# NVIDIA DRS declarations

`nvapi_drs.h` contains the DRS data declarations extracted from NVIDIA's MIT
licensed `nvapi.h`, pinned to commit
`70d337db9186e968eab622f7e786de7e437faf3d` of
https://github.com/NVIDIA/nvapi. Unused SDK declarations and dependencies are
omitted; integer aliases and the version macro are supplied locally. Function
interface IDs used by the adapter come from `nvapi_interface.h` at that commit;
`PREFERRED_PSTATE_ID` and `PREFERRED_PSTATE_PREFER_MAX` come from
`NvApiDriverSettings.h`. The old, supported DRS structure versions are retained
for R500-series driver compatibility. No NVIDIA DLL or import library is bundled.

See `LICENSE.txt`. The helper only loads the installed System32 NVAPI DLL.
