// Compact production texture ABI, descriptor-page and delayed GPU lifetime proof.
// Run serially through tools/game_slot.py.
#include "gfx/translated_textures.hpp"
#include "gfx/translated_samplers.hpp"
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace sr::gfx;
namespace {
unsigned passed = 0, failed = 0;
void check(bool ok, const char* name) {
  std::printf("[TRANSLATED_TEXTURES_TEST] %s %s\n", ok ? "PASS" : "FAIL", name);
  ok ? ++passed : ++failed;
}
void require(HRESULT hr, const char* name) {
  if (FAILED(hr)) { std::fprintf(stderr,"SETUP FAILED %s hr=%08lX\n",name,(unsigned long)hr); std::exit(2); }
}
void transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
  commands->ResourceBarrier(1,&b);
}
NativeViewRecipe nullTexture(D3D12_SRV_DIMENSION dimension) {
  NativeViewRecipe r; r.srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  r.srv.ViewDimension = dimension; r.srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (dimension == D3D12_SRV_DIMENSION_TEXTURE2D) r.srv.Texture2D.MipLevels = 1;
  else if (dimension == D3D12_SRV_DIMENSION_TEXTURE3D) r.srv.Texture3D.MipLevels = 1;
  else r.srv.TextureCube.MipLevels = 1;
  return r;
}
void arenaChecks(ID3D12Device* device) {
  NativeDescriptorArena arena; NativeDescriptorVersion first, second, output;
  auto recipe = nullTexture(D3D12_SRV_DIMENSION_TEXTURE2D);
  check(arena.create({&recipe,1},output) == E_ABORT && !output,"uninitialized_arena_preserves_output");
  check(arena.initialize(device,0,1) == E_INVALIDARG,"zero_page_size_rejected");
  require(arena.initialize(device,2,1),"bounded arena");
  require(arena.create({&recipe,1},first),"first range");
  require(arena.create({&recipe,1},second),"second range");
  const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  check(first.heap() == second.heap() && second.gpu().ptr == first.gpu().ptr + stride,
        "same_page_versions_have_distinct_gpu_offsets");
  output = second;
  check(arena.create({&recipe,1},output) == E_OUTOFMEMORY && output.gpu().ptr == second.gpu().ptr,
        "held_page_budget_refuses_without_replacing_output");
  auto bad = recipe; bad.srv.Texture2D.MipLevels = 0;
  check(arena.create({&bad,1},output) == E_INVALIDARG && output.gpu().ptr == second.gpu().ptr,
        "invalid_view_preserves_published_output");
  auto owner = first.ownership(); first = {}; second = {}; output = {};
  check(owner.expired(),"released_version_retires_independently_of_tail");
  std::array<NativeViewRecipe,2> recipes{recipe,recipe};
  require(arena.create(recipes,output),"returned page budget");
  check(arena.livePages() == 1 && output.count() == 2,"final_version_release_returns_page_budget");
  arena.discardTail();
  check(arena.livePages() == 1,"discarding_tail_preserves_outstanding_versions");
  output = {}; check(arena.livePages() == 0,"last_owner_releases_page");
  // Pages are ranges of one heap: a version on the second page binds with one
  // on the first, and a recycled range reuses that heap instead of a new one.
  NativeDescriptorArena shared; NativeDescriptorVersion p0, p1, again;
  require(shared.initialize(device,1,2),"two single-descriptor pages");
  require(shared.create({&recipe,1},p0),"page 0");
  require(shared.create({&recipe,1},p1),"page 1");
  check(shared.livePages() == 2 && p0.heap() == p1.heap() && p1.gpu().ptr == p0.gpu().ptr + stride,
        "pages_are_ranges_of_one_heap");
  auto heap = p0.heap(); auto first0 = p0.gpu().ptr; p0 = {}; shared.discardTail();
  require(shared.create({&recipe,1},again),"recycled range");
  check(again.heap() == heap && again.gpu().ptr == first0 && again.gpu().ptr != p1.gpu().ptr,
        "expired_range_is_reused_in_the_same_heap");
}
// A hot table whose page falls behind the tail migrates to the tail once, then
// hits again; tables are never rewritten in place (the old version stays valid).
void cacheAging(ID3D12Device* device) {
  std::array<NativeViewRecipe,48> views;
  for(uint32_t i=0;i<16;++i){views[i]=nullTexture(D3D12_SRV_DIMENSION_TEXTURE2D);
    views[16+i]=nullTexture(D3D12_SRV_DIMENSION_TEXTURE3D);views[32+i]=nullTexture(D3D12_SRV_DIMENSION_TEXTURECUBE);}
  NativeViewRecipe raw;raw.srv.Format=DXGI_FORMAT_R32_TYPELESS;raw.srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
  raw.srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;raw.srv.Buffer={0,16,0,D3D12_BUFFER_SRV_FLAG_RAW};
  TranslatedTextureCache cache;TranslatedTextureSnapshot hot,other,again,settled;
  require(cache.select(device,views,raw,hot),"hot table");const auto firstPage=hot.descriptors.pageSerial();
  // 6+ pages of distinct tables (256 tables of 64 descriptors per 16384-descriptor page).
  auto mapping=[](uint32_t n){return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(n%6,(n/6)%6,(n/36)%6,(n/216)%6);};
  uint32_t made=0;
  for(uint32_t n=1;n<1296*2&&other.descriptors.pageSerial()<firstPage+6;++n){
    // Slot 0 takes the first 1296 swizzles, slot 1 the next: every set is distinct.
    auto v=views;auto& m=v[n/1296].srv.Shader4ComponentMapping;m=mapping(n%1296);
    if(m==D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING)continue;
    require(cache.select(device,v,raw,other),"filler table");++made;
  }
  std::printf("[TRANSLATED_TEXTURES_TEST] aging made=%u misses=%llu hits=%llu firstPage=%llu otherPage=%llu live=%zu\n",made,
      (unsigned long long)cache.tableMisses(),(unsigned long long)cache.tableHits(),(unsigned long long)firstPage,
      (unsigned long long)other.descriptors.pageSerial(),cache.livePages());
  check(other.descriptors.pageSerial()>=firstPage+6&&cache.tableMisses()==made+1,"filler_tables_advance_the_tail_six_pages");
  const auto migrations=cache.tableMigrations();
  require(cache.select(device,views,raw,again),"aged hit");
  check(cache.tableMigrations()==migrations+1&&again.descriptors.gpu().ptr!=hot.descriptors.gpu().ptr&&
        again.descriptors.pageSerial()>=firstPage+6&&hot.descriptors.pageSerial()==firstPage,
        "aged_hit_migrates_to_the_tail_and_keeps_the_old_version");
  const auto hits=cache.tableHits();
  require(cache.select(device,views,raw,settled),"settled hit");
  check(cache.tableHits()==hits+1&&settled.descriptors.gpu().ptr==again.descriptors.gpu().ptr,"migrated_table_hits_again");
}
std::array<uint8_t,4> texel(uint32_t layer,uint32_t z,uint32_t mip,uint8_t variant) {
  return {uint8_t(17 + layer*19 + mip*71 + variant),uint8_t(31 + z*47 + mip*23),
          uint8_t(83 + layer*13 + z*11 + mip*29),255};
}
NativeViewRecipe texture(ID3D12Device* device,NativeDrawRecording& recording,NativeUploadArena& uploads,
                         D3D12_SRV_DIMENSION dimension,uint16_t layers,uint8_t variant) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = dimension == D3D12_SRV_DIMENSION_TEXTURE3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = desc.Height = 4; desc.DepthOrArraySize = layers; desc.MipLevels = 2;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT; NativeViewRecipe view;
  require(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,
      nullptr,IID_PPV_ARGS(&view.resource)),"texture");
  const UINT count = desc.MipLevels * (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : layers);
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
  std::vector<UINT> rows(count); UINT64 total = 0;
  device->GetCopyableFootprints(&desc,0,count,0,footprints.data(),rows.data(),nullptr,&total);
  std::vector<uint8_t> bytes(size_t(total),0);
  for (UINT sub=0;sub<count;++sub) {
    const auto& f=footprints[sub];
    for (UINT z=0;z<f.Footprint.Depth;++z) for (UINT y=0;y<rows[sub];++y) for (UINT x=0;x<f.Footprint.Width;++x) {
      auto color=texel(sub/2,z,sub%2,variant);
      std::memcpy(bytes.data()+f.Offset+(uint64_t(z)*rows[sub]+y)*f.Footprint.RowPitch+x*4,color.data(),4);
    }
  }
  NativeBufferSlice source; require(uploads.stage(bytes,512,source),"texture bytes");
  require(recording.keep(source),"texture upload retention"); require(recording.keep(view.resource.Get()),"texture retention");
  for (UINT sub=0;sub<count;++sub) {
    D3D12_TEXTURE_COPY_LOCATION from{},to{}; from.pResource=source.resource(); from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint=footprints[sub]; from.PlacedFootprint.Offset+=source.offset();
    to.pResource=view.resource.Get(); to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; to.SubresourceIndex=sub;
    recording.commands()->CopyTextureRegion(&to,0,0,0,&from,nullptr);
  }
  transition(recording.commands(),view.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  view.srv.Format=desc.Format; view.srv.ViewDimension=dimension; view.srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (dimension==D3D12_SRV_DIMENSION_TEXTURE2D) view.srv.Texture2D.MipLevels=2;
  else if (dimension==D3D12_SRV_DIMENSION_TEXTURE2DARRAY) { view.srv.Texture2DArray.MipLevels=2; view.srv.Texture2DArray.ArraySize=layers; }
  else if (dimension==D3D12_SRV_DIMENSION_TEXTURE3D) view.srv.Texture3D.MipLevels=2;
  else view.srv.TextureCube.MipLevels=2;
  return view;
}
constexpr char shader[] = R"(
  Texture2D<float4> plain[32] : register(t0,space0);
  Texture3D<float4> legacyVolume[16] : register(t0,space1);
  TextureCube<float4> legacyCube[16] : register(t0,space2);
  SamplerState samplers[16] : register(s0,space3);
  ByteAddressBuffer raw : register(t0,space5);
  Texture2DArray<float4> arrays[16] : register(t0,space6);
  Texture3D<float4> volumes[16] : register(t0,space7);
  TextureCube<float4> cubes[16] : register(t0,space8);
  cbuffer Params : register(b0,space4) { uint mode; uint key; uint face; uint unused; };
  float4 VS(uint id : SV_VertexID) : SV_Position { return float4(id==1?3:-1,id==2?-3:1,.5,1); }
  float3 direction(uint f) {
    if(f==0)return float3(1,0,0); if(f==1)return float3(-1,0,0);
    if(f==2)return float3(0,1,0); if(f==3)return float3(0,-1,0);
    if(f==4)return float3(0,0,1); return float3(0,0,-1);
  }
  float4 PS() : SV_Target {
    uint i=key&0x1fffffff;
    if(mode==0)return plain[i].Load(int3(0,0,0));
    if(mode==1) {
      if(key&0x20000000)return volumes[i].Load(int4(0,0,1,0));
      if(key&0x40000000)return arrays[i].Load(int4(0,0,1,0));
      return plain[i].Load(int3(0,0,0));
    }
    if(mode==2)return cubes[i].SampleLevel(samplers[0],direction(face),0);
    if(mode==3)return legacyCube[i].SampleLevel(samplers[0],direction(face),0);
    if(mode==4) { uint v=raw.Load(key); return float4(v&255,(v>>8)&255,(v>>16)&255,255)/255.0; }
    return legacyVolume[i].Load(int4(0,0,1,0));
  }
)";
NativeShaderCode compile(const char* entry,const char* profile) {
  ComPtr<ID3DBlob> blob,errors;
  HRESULT hr=D3DCompile(shader,std::strlen(shader),"translated_texture_fixture",nullptr,nullptr,entry,profile,
      D3DCOMPILE_ENABLE_STRICTNESS,0,&blob,&errors);
  if(FAILED(hr)&&errors)std::fprintf(stderr,"%s\n",(const char*)errors->GetBufferPointer());
  require(hr,entry); NativeShaderCode code;
  require(NativeShaderCode::copy({(const uint8_t*)blob->GetBufferPointer(),blob->GetBufferSize()},code),"shader copy");return code;
}
void delayedPixels(ID3D12Device* device,ID3D12CommandQueue* queue,NativeImageStore& images) {
  D3D12_ROOT_PARAMETER params[15]{}; D3D12_DESCRIPTOR_RANGE ranges[8]{}; std::array<uint32_t,15> extents{};
  params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[0].Constants={0,4,4};
  for(uint32_t i: {1u,2u,3u,4u,9u}) { params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV; params[i].Descriptor={i,9};extents[i]=256; }
  const uint32_t slots[8]={5,6,7,8,10,11,12,13}, spaces[8]={0,1,2,3,5,6,7,8},counts[8]={32,16,16,16,1,16,16,16};
  for(uint32_t i=0;i<8;++i) {
    ranges[i]={i==3?D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:D3D12_DESCRIPTOR_RANGE_TYPE_SRV,counts[i],0,spaces[i],0};
    params[slots[i]].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[slots[i]].DescriptorTable={1,&ranges[i]};
  }
  params[14].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[14].Constants={6,4,2};
  D3D12_ROOT_SIGNATURE_DESC rd{15,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE}; NativeRootSignature root;
  require(NativeRootSignature::create(device,rd,root,extents),"ABI root");
  NativePipelineDesc pd;pd.rootSignature=root.root();pd.vertex=compile("VS","vs_5_1");pd.pixel=compile("PS","ps_5_1");
  pd.colorCount=1;pd.colorFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;pd.blend.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.rasterizer.FillMode=D3D12_FILL_MODE_SOLID;pd.rasterizer.CullMode=D3D12_CULL_MODE_NONE;pd.rasterizer.DepthClipEnable=TRUE;
  pd.depthStencil.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
  pd.depthStencil.FrontFace=pd.depthStencil.BackFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
  NativePipelineCache pipelines;require(pipelines.initialize(device),"pipeline cache");NativePipelineLease pipeline;
  require(pipelines.acquire(pd,pipeline),"pipeline");
  NativeImageDesc targetDesc;targetDesc.width=174;targetDesc.height=1;targetDesc.resourceFormat=targetDesc.clear.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
  targetDesc.initialState=D3D12_RESOURCE_STATE_RENDER_TARGET;NativeImageLease target;require(images.create(targetDesc,target),"target");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 total=0;auto td=target.resource()->GetDesc();
  device->GetCopyableFootprints(&td,0,1,0,&footprint,nullptr,nullptr,&total);
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC bd{};
  bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=total;bd.Height=bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> readback;require(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"readback");
  std::array<NativeViewRecipe,48> views; NativeUploadArena uploads;require(uploads.initialize(device,65536,1u<<20),"uploads");
  {
    NativeDrawRecording init;require(init.begin(device),"initialization");
    auto plain=texture(device,init,uploads,D3D12_SRV_DIMENSION_TEXTURE2D,1,1);
    auto layered=texture(device,init,uploads,D3D12_SRV_DIMENSION_TEXTURE2D,1,40);
    auto array=texture(device,init,uploads,D3D12_SRV_DIMENSION_TEXTURE2DARRAY,2,2);
    auto volume=texture(device,init,uploads,D3D12_SRV_DIMENSION_TEXTURE3D,2,3);
    auto cube=texture(device,init,uploads,D3D12_SRV_DIMENSION_TEXTURECUBE,6,4);
    for(uint32_t i=0;i<16;++i){views[i]=plain;views[16+i]=i%3==0?layered:i%3==1?array:volume;views[32+i]=cube;}
    views[15]=nullTexture(D3D12_SRV_DIMENSION_TEXTURE2D);
    require(init.close(),"initialization close");uint64_t ticket=0;require(init.submit(images,ticket),"initialization submit");
    require(images.wait(ticket,10000),"initialization complete");require(images.collect(),"initialization collect");
  }
  ComPtr<ID3D12Fence> delay;require(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&delay)),"delay");
  require(queue->Wait(delay.Get(),1),"delay GPU");
  std::vector<std::array<uint8_t,4>> expected;std::weak_ptr<const void> firstOwner,secondOwner,rawOwner;uint64_t ticket=0;
  {
    NativeDrawRecording recording;require(recording.begin(device),"drawing");
    require(recording.keep(target),"target retention");require(recording.keep(readback.Get()),"readback retention");
    std::array<uint8_t,64> prefix{};NativeBufferSlice prefixSlice;require(uploads.stage(prefix,16,prefixSlice),"prefix");
    std::array<uint32_t,512> source{};source[0]=0x000B1621;source[3]=0x005A5046;NativeBufferSlice rawSlice;
    require(uploads.stage({(const uint8_t*)source.data(),sizeof(source)},16,rawSlice),"raw snapshot");rawOwner=rawSlice.ownership();
    check(rawSlice.offset()!=0,"raw_view_uses_nonzero_owned_buffer_offset");
    NativeViewRecipe raw;raw.buffer=rawSlice;raw.srv.Format=DXGI_FORMAT_R32_TYPELESS;raw.srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
    raw.srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;raw.srv.Buffer={rawSlice.offset()/4,UINT(rawSlice.size()/4),0,D3D12_BUFFER_SRV_FLAG_RAW};
    TranslatedTextureCache cache;TranslatedTextureSnapshot a,b,current;require(cache.select(device,views,raw,a),"first table");firstOwner=a.descriptors.ownership();
    require(cache.select(device,views,raw,current),"reuse");check(a.descriptors.gpu().ptr==current.descriptors.gpu().ptr,"unchanged_recipes_reuse_version");
    check(a.raw.gpu().ptr==current.raw.gpu().ptr&&a.raw.heap()==a.descriptors.heap(),"unchanged_raw_reuses_view_in_the_table_heap");
    {
      auto moved=raw;moved.srv.Buffer.NumElements-=1;TranslatedTextureSnapshot c;require(cache.select(device,views,moved,c),"raw change");
      check(c.descriptors.gpu().ptr==a.descriptors.gpu().ptr&&c.raw.gpu().ptr!=a.raw.gpu().ptr,"raw_change_reuses_texture_table");
    }
    views[0].srv.Texture2D.MostDetailedMip=1;views[0].srv.Texture2D.MipLevels=1;
    require(cache.select(device,views,raw,b),"mip change");secondOwner=b.descriptors.ownership();
    check(a.descriptors.heap()==b.descriptors.heap()&&a.descriptors.gpu().ptr!=b.descriptors.gpu().ptr,"srv_change_gets_distinct_same_page_version");
    auto handle=b.descriptors.gpu().ptr;auto bad=views;bad[0].srv.Texture2D.MipLevels=0;
    check(cache.select(device,bad,raw,b)==E_INVALIDARG&&b.descriptors.gpu().ptr==handle,"invalid_cache_selection_preserves_output");
    auto* commands=recording.commands();commands->SetGraphicsRootSignature(root.root());commands->SetPipelineState(pipeline.pipeline());
    auto rtv=target.view();commands->OMSetRenderTargets(1,&rtv,FALSE,nullptr);commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    TranslatedSamplerCache samplerCache;NativeDescriptorVersion samplers;require(samplerCache.select(device,0,0,samplers),"samplers");
    auto draw=[&](uint32_t mode,uint32_t key,uint32_t face,std::array<uint8_t,4> color){
      auto x=LONG(expected.size());expected.push_back(color);D3D12_VIEWPORT viewport{float(x),0,1,1,0,1};D3D12_RECT scissor{x,0,x+1,1};
      commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&scissor);uint32_t constants[4]={mode,key,face,0};
      commands->SetGraphicsRoot32BitConstants(0,4,constants,0);commands->DrawInstanced(3,1,0,0);
    };
    for(uint32_t phase=0;phase<3;++phase){
      const auto& selected=phase==1?b:a;require(selected.bind(recording,samplers),"complete ABI bind");
      for(uint32_t i=0;i<16;++i)draw(0,selected.indices[i],0,i==15?std::array<uint8_t,4>{0,0,0,0}:texel(0,0,phase==1&&i==0?1:0,1));
      for(uint32_t i=0;i<16;++i)draw(1,selected.indices[16+i],0,i%3==0?texel(0,0,0,40):i%3==1?texel(1,0,0,2):texel(0,1,0,3));
      for(uint32_t i=0;i<16;++i)draw(2,selected.indices[32+i],i%6,texel(i%6,0,0,4));
      for(uint32_t face=0;face<6;++face)draw(3,0,face,texel(face,0,0,4));
      draw(4,0,0,{33,22,11,255});draw(4,12,0,{70,80,90,255});draw(5,2,0,texel(0,1,0,3));
    }
    check(expected.size()==171,"all_pixel_cases_recorded");
    // Evict both published versions. The recording must keep each allocation,
    // even when they share a heap; keeping only the page is insufficient.
    for(uint32_t i=0;i<130;++i){raw.srv.Buffer.NumElements=16+i;require(cache.select(device,views,raw,current),"cache eviction");}
    source.fill(0);views={};raw={};rawSlice={};current={};a={};b={};cache.clear();uploads.discardTail();
    transition(commands,target.resource(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=target.resource();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
    commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);require(recording.close(),"close");require(recording.submit(images,ticket),"submit");
  }
  check(images.wait(ticket,0)==HRESULT_FROM_WIN32(WAIT_TIMEOUT),"GPU_queue_is_actually_delayed");
  bool alive=!firstOwner.expired()&&!secondOwner.expired()&&!rawOwner.expired();check(alive,"same_page_versions_and_raw_survive_cache_and_producer_destruction");
  if(!alive){std::puts("[TRANSLATED_TEXTURES_TEST] ABORT dangling queued version");std::exit(1);}
  require(delay->Signal(1),"release GPU");require(images.wait(ticket,10000),"completion");require(images.collect(),"collect");
  uint8_t* data=nullptr;D3D12_RANGE range{0,size_t(total)};require(readback->Map(0,&range,reinterpret_cast<void**>(&data)),"map");
  bool exact=true;for(size_t i=0;i<expected.size();++i)if(std::memcmp(data+footprint.Offset+i*4,expected[i].data(),4)!=0){
    exact=false;std::printf("PIXEL_FAIL index=%zu actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n",i,data[i*4],data[i*4+1],data[i*4+2],data[i*4+3],expected[i][0],expected[i][1],expected[i][2],expected[i][3]);
  }
  D3D12_RANGE noWrites{0,0};readback->Unmap(0,&noWrites);check(exact,"all_171_ABA_texture_and_raw_pixels_exact_after_owner_destruction");
  check(firstOwner.expired()&&secondOwner.expired()&&rawOwner.expired(),"all_versions_retire_after_checked_completion");
  require(images.release(target.handle()),"release target");
}
}
int main(){
  ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
  ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
  require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");require(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"WARP");
  require(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");D3D12_COMMAND_QUEUE_DESC q{};
  require(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");arenaChecks(device.Get());cacheAging(device.Get());
  NativeImageStore images;require(images.initialize(device.Get(),queue.Get(),4,4),"images");delayedPixels(device.Get(),queue.Get(),images);require(images.shutdown(),"shutdown");
  ComPtr<ID3D12InfoQueue> validation;if(debug&&SUCCEEDED(device.As(&validation))){
    bool clean=true;for(UINT64 i=0;i<validation->GetNumStoredMessages();++i){SIZE_T size=0;require(validation->GetMessage(i,nullptr,&size),"message size");
      std::vector<uint8_t> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());require(validation->GetMessage(i,message,&size),"message");
      if(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR||message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION){clean=false;std::fprintf(stderr,"D3D12 ERROR %s\n",message->pDescription);}
    }check(clean,"debug_layer_reports_no_error_or_corruption");
  }
  std::printf("[TRANSLATED_TEXTURES_TEST] RESULT checks=%u passed=%u failed=%u\n",passed+failed,passed,failed);return failed?1:0;
}
