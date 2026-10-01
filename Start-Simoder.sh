#!/usr/bin/env bash
set -euo pipefail

# /** Resolves the installed game root without depending on the caller's working directory. */
simoder_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
simoder_exe="${simoder_root}/simoder/simoder.exe"
simoder_dll="${simoder_root}/simoder/sc13modloader.dll"
simcity_uri='origin2://game/launch/?offerIds=71654,71480,71630,71650,71573,71631,71652,71572,71632,1004769,1004768,1004771,1004770,1008749,1008760,1008761,1008762,1008763,1008764,1008776,1008777,1008778,1008779,1015233,1015232,1015226'

# /** Rejects an incomplete installation before starting Wine or Proton. */
if [[ ! -f "${simoder_exe}" || ! -f "${simoder_dll}" ]]; then
  echo "Simoder runtime is incomplete under ${simoder_root}/simoder." >&2
  exit 1
fi

# /** Runs the Windows x86 watcher and EA URI in one explicitly selected compatibility runtime. */
if [[ -n "${SIMODER_PROTON:-}" ]]; then
  "${SIMODER_PROTON}" run "${simoder_exe}" --watch-attach 4294967295 "${simoder_dll}" &
  "${SIMODER_PROTON}" run explorer.exe "${simcity_uri}"
else
  simoder_wine="${SIMODER_WINE:-wine}"
  "${simoder_wine}" "${simoder_exe}" --watch-attach 4294967295 "${simoder_dll}" &
  "${simoder_wine}" start "${simcity_uri}"
fi
