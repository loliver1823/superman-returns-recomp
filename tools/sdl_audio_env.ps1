# SDL audio env names for harness launches: SDL2 and SDL3 spell them differently (tools/sdl_audio_env.py has the
# table and the reasons). Dot-source it:  . (Join-Path $PSScriptRoot 'sdl_audio_env.ps1')
#   Add-SrSdlAudioEnvMirror $psi.EnvironmentVariables   # a ProcessStartInfo's env (bg_window.ps1 Start-SrGameProcess)
#   Add-SrSdlAudioEnvMirror                             # this session's $env: (no argument)
#   Get-SrSdlAudioDiskGates [-OutFile <raw>]            # 'SDL_AUDIODRIVER=disk,SDL_AUDIO_DRIVER=disk[,...]'
# Self-test: powershell -NoProfile -File tools/sdl_audio_env.ps1 -SrSdlAudioEnvSelfTest
param([switch]$SrSdlAudioEnvSelfTest)   # unique name: this file is dot-sourced into other scripts

$script:SrSdlAudioEnvPairs = @(
    @('SDL_AUDIODRIVER', 'SDL_AUDIO_DRIVER'),
    @('SDL_DISKAUDIOFILE', 'SDL_AUDIO_DISK_OUTPUT_FILE')
)

# Fill in the missing spelling of each pair from the one that is set. $Target = a StringDictionary
# (ProcessStartInfo.EnvironmentVariables) or a hashtable ($Target); omitted = this session's environment. A pair whose
# two names are both set is left alone.
function Add-SrSdlAudioEnvMirror($Target = $null) {
    # (after the pairs) the disk driver with no output file named writes NUL, not sdlaudio.raw in the working
    # directory (SDL2 and SDL3 default; ~250 MB of a roll landed in the dev checkout, 2026-10-18)
    foreach ($p in $script:SrSdlAudioEnvPairs) {
        $old = $p[0]; $new = $p[1]
        if ($null -eq $Target) {
            $vo = [Environment]::GetEnvironmentVariable($old); $vn = [Environment]::GetEnvironmentVariable($new)
            if ($vo -and -not $vn) { [Environment]::SetEnvironmentVariable($new, $vo) }
            elseif ($vn -and -not $vo) { [Environment]::SetEnvironmentVariable($old, $vn) }
        } else {
            $vo = $Target[$old]; $vn = $Target[$new]
            if ($vo -and -not $vn) { $Target[$new] = $vo }
            elseif ($vn -and -not $vo) { $Target[$old] = $vn }
        }
    }
    if ($null -eq $Target) {
        $drv = [Environment]::GetEnvironmentVariable('SDL_AUDIO_DRIVER'); if (-not $drv) { $drv = [Environment]::GetEnvironmentVariable('SDL_AUDIODRIVER') }
        $file = [Environment]::GetEnvironmentVariable('SDL_AUDIO_DISK_OUTPUT_FILE'); if (-not $file) { $file = [Environment]::GetEnvironmentVariable('SDL_DISKAUDIOFILE') }
        if ("$drv".Trim().ToLower() -eq 'disk' -and -not $file) {
            [Environment]::SetEnvironmentVariable('SDL_AUDIO_DISK_OUTPUT_FILE', 'NUL'); [Environment]::SetEnvironmentVariable('SDL_DISKAUDIOFILE', 'NUL')
        }
    } else {
        $drv = $Target['SDL_AUDIO_DRIVER']; if (-not $drv) { $drv = $Target['SDL_AUDIODRIVER'] }
        $file = $Target['SDL_AUDIO_DISK_OUTPUT_FILE']; if (-not $file) { $file = $Target['SDL_DISKAUDIOFILE'] }
        if ("$drv".Trim().ToLower() -eq 'disk' -and -not $file) { $Target['SDL_AUDIO_DISK_OUTPUT_FILE'] = 'NUL'; $Target['SDL_DISKAUDIOFILE'] = 'NUL' }
    }
}

function Get-SrSdlAudioDiskGates([string]$OutFile = "", [string]$Driver = "disk") {
    $g = @("SDL_AUDIODRIVER=$Driver", "SDL_AUDIO_DRIVER=$Driver")
    if ($OutFile) { $g += @("SDL_DISKAUDIOFILE=$OutFile", "SDL_AUDIO_DISK_OUTPUT_FILE=$OutFile") }
    return ($g -join ",")
}

if ($SrSdlAudioEnvSelfTest) {
    $fails = 0
    function Check([bool]$ok, [string]$name) { Write-Output ((@('FAIL ', 'PASS ')[[int]$ok]) + $name); if (-not $ok) { $script:fails++ } }
    $h = @{ SDL_AUDIODRIVER = 'disk' }; Add-SrSdlAudioEnvMirror $h
    Check ($h.SDL_AUDIO_DRIVER -eq 'disk') 'SDL2 name only -> SDL3 name added (hashtable)'
    Check ($h.SDL_AUDIO_DISK_OUTPUT_FILE -eq 'NUL' -and $h.SDL_DISKAUDIOFILE -eq 'NUL') 'disk with no file named -> NUL (both spellings)'
    $h = @{ SDL_AUDIO_DRIVER = 'disk'; SDL_DISKAUDIOFILE = 'o.raw' }; Add-SrSdlAudioEnvMirror $h
    Check ($h.SDL_AUDIO_DISK_OUTPUT_FILE -eq 'o.raw') 'a named disk file is kept'
    $h = @{ SDL_AUDIO_DRIVER = 'dummy'; SDL_AUDIO_DISK_OUTPUT_FILE = 'x.raw' }; Add-SrSdlAudioEnvMirror $h
    Check ($h.SDL_AUDIODRIVER -eq 'dummy' -and $h.SDL_DISKAUDIOFILE -eq 'x.raw') 'SDL3 names only -> SDL2 names added'
    $h = @{ SDL_AUDIODRIVER = 'disk'; SDL_AUDIO_DRIVER = 'wasapi' }; Add-SrSdlAudioEnvMirror $h
    Check ($h.SDL_AUDIODRIVER -eq 'disk' -and $h.SDL_AUDIO_DRIVER -eq 'wasapi') 'both set: left alone'
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.EnvironmentVariables['SDL_AUDIODRIVER'] = 'disk'; $psi.EnvironmentVariables.Remove('SDL_AUDIO_DRIVER')
    Add-SrSdlAudioEnvMirror $psi.EnvironmentVariables
    Check ($psi.EnvironmentVariables['SDL_AUDIO_DRIVER'] -eq 'disk') 'ProcessStartInfo env: SDL3 name added'
    Check ((Get-SrSdlAudioDiskGates -OutFile 'C:\o\a.raw') -eq 'SDL_AUDIODRIVER=disk,SDL_AUDIO_DRIVER=disk,SDL_DISKAUDIOFILE=C:\o\a.raw,SDL_AUDIO_DISK_OUTPUT_FILE=C:\o\a.raw') 'disk gates: both spellings'
    Write-Output ("[SDL_AUDIO_ENV_TEST] {0} failed" -f $fails)
    exit $fails
}
