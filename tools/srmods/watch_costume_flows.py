"""Read-only, non-atomic flow/DAS sampling of an already running recomp.

No process launch, guest calls, writes, thread suspension or input injection.
Run with the Python that has Frida, and redirect stdout to an artifact JSONL.
Offsets are retail-specific; missing/unmapped fields are errors, never zeroes.
"""
import argparse
import json
import time

AGENT = r"""
const base = Module.findGlobalExportByName('g_debugGuestBase').readPointer();
function bytes(va,n) {
  const a=base.add(va>>>0), r=Process.findRangeByAddress(a);
  if (!r || !r.protection.startsWith('r') || a.add(n).compare(r.base.add(r.size))>0)
    throw Error('unmapped 0x'+(va>>>0).toString(16));
  return new Uint8Array(a.readByteArray(n));
}
function u32(va) {
  const b=bytes(va,4);
  return (b[0]*0x1000000+(b[1]<<16)+(b[2]<<8)+b[3])>>>0;
}
function hex(n) { return '0x'+(n>>>0).toString(16).padStart(8,'0'); }
const events=[];
function luaArgs(state) {
  const start=u32(state+12), top=u32(state+8), result=[];
  if (top<start || top-start>512 || (top-start)%8!==0) throw Error('invalid Lua stack');
  for (let p=start;p<top && result.length<3;p+=8) {
    const tag=u32(p), raw=u32(p+4); let value=hex(raw);
    if (tag===4) {
      const b=bytes(raw+16,160); value='';
      for (let i=0;i<b.length && b[i];i++) value+=String.fromCharCode(b[i]);
    } else if (tag===3) {
      const b=bytes(p+4,4); value=new DataView(b.buffer).getFloat32(0,false);
    }
    result.push({tag:tag,value:value});
  }
  return result;
}
rpc.exports.watchcalls = function() {
  const table=Module.findGlobalExportByName('g_debugDispatchTable').readPointer();
  const code=Module.findGlobalExportByName('g_debugDispatchCodeBase').readU32();
  const count=Module.findGlobalExportByName('g_debugDispatchTableEntries').readU64().toNumber();
  const gpr=Module.findGlobalExportByName('g_debugCtxGprOff').add(3*4).readU32();
  const lr=Module.findGlobalExportByName('g_debugCtxLrOff').readU32();
  const hooked=[];
  [0x82207fc0,0x821cced0].forEach(function(va) {
    const index=(va-code)/4;
    if (!Number.isInteger(index) || index<0 || index>=count) throw Error('invalid dispatch index');
    const fn=table.add(index*Process.pointerSize).readPointer();
    if (fn.isNull()) throw Error('missing dispatch '+hex(va));
    let seen=0;
    Interceptor.attach(fn,{onEnter:function(args) {
      if (++seen>512) return;
      try { events.push({time:Date.now(),call:hex(va),ordinal:seen,
        lr:hex(args[0].add(lr).readU32()),args:luaArgs(args[0].add(gpr).readU32())}); }
      catch(e) { events.push({time:Date.now(),call:hex(va),error:String(e)}); }
    }});
    hooked.push({guest:hex(va),host:fn.toString(),cap:512});
  });
  return hooked;
};
rpc.exports.events = function() { return events.splice(0,events.length); };
rpc.exports.snapshot = function() {
  const mgr=u32(0x8296df74), ui=u32(0x82911460), mode=u32(0x8296df4c);
  const out={time:Date.now(),mgr:hex(mgr),uiRun:ui?u32(ui+0x48):null,
    modeState:mode?u32(mode+8):null,hud:hex(u32(0x8296ee80)),flows:[]};
  const slots=mgr?u32(mgr+0x20):0;
  if (!slots) return out;
  for (let i=0;i<8;i++) {
    const f=u32(slots+4*i); if (!f) continue;
    try {
      const fm=u32(f+0x6c), das=u32(f+0x68), fl=bytes(f+0x29c,4);
      const row={slot:i,va:hex(f),nameHash:hex(u32(f+0x4c)),
        preload:bytes(f+0x98,1)[0],spawned:bytes(f+0x99,1)[0],
        loaded:fl[0],spawnRequested:fl[1],paused:fl[2],active:fl[3],
        fsm:hex(fm),index:fm?u32(fm+8):null,previous:fm?u32(fm+12):null,
        stateCount:fm?u32(fm+0x18):null,das:hex(das),costumeRequests:[]};
      if (das) {
        const root=u32(das+4), start=root?u32(root):0, end=root?u32(root+4):0;
        row.dasRoot=hex(root);
        if (start && end>=start && end-start<=4096 && (end-start)%4===0) {
          for (let p=start;p<end;p+=4) {
            const node=u32(p);
            if (node && u32(node)===0x2e196d85)
              row.costumeRequests.push({va:hex(node),value:u32(node+4),flags:hex(u32(node+8))});
          }
        } else row.dasVectorError='invalid or unavailable vector';
      }
      out.flows.push(row);
    } catch (e) { out.flows.push({slot:i,va:hex(f),error:String(e)}); }
  }
  return out;
};
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--pid', type=int, required=True)
    ap.add_argument('--seconds', type=int, default=300)
    ap.add_argument('--calls', action='store_true', help='observe two native Lua commands (512 entries each)')
    args = ap.parse_args()
    if args.pid <= 0 or not 1 <= args.seconds <= 540:
        ap.error('positive PID and duration 1..540 seconds required')
    import frida
    session = frida.attach(args.pid)
    script = session.create_script(AGENT)
    try:
        script.load()
        if args.calls:
            print(json.dumps({'hooks': script.exports_sync.watchcalls()}), flush=True)
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            print(json.dumps(script.exports_sync.snapshot(), separators=(',', ':')), flush=True)
            for event in script.exports_sync.events():
                print(json.dumps(event, separators=(',', ':')), flush=True)
            time.sleep(1)
    except frida.InvalidOperationError as exc:
        print(json.dumps({'observer_end': str(exc)}), flush=True)
    finally:
        try:
            script.unload()
            session.detach()
        except frida.InvalidOperationError:
            pass


if __name__ == '__main__':
    main()
