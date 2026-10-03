# Third-party notices

AFAR RX Calibration Studio runtime includes the following third-party components:

- Qt 6 (Qt Core, GUI, Widgets and platform plugins), available under LGPLv3/GPLv3
  or a commercial Qt license. Exact license texts shipped by the MSYS2 package are
  included in `licenses/qt6-base/`.
- GCC/MinGW-w64 runtime libraries (`libgcc`, `libstdc++`, `libwinpthread`). Runtime
  exception and license texts are included in `licenses/gcc-libs/`.
- nlohmann/json 3.11.3, MIT License. Its license is included as
  `licenses/nlohmann-json-LICENSE.MIT`.

Catch2 is used by the test build and is not part of the runtime archive.
