# Gamepad Slotter

Force a game controller to use a given index (LED number) when plugged in on Windows.
It can be used to enforce the order of multiple controllers as well.

The order of controllers on Windows is unreliable.
Unplugging and replugging a controller may not be enough to have it use the first slot.
Unfortunately, some applications and games will only work with a controller in the first slot.


## How to use

* Run `gamepad-slotter N` were `N` is the target slot, from 1 to 4 (default is 1).
* Plug a controller in the target slot.
* Wait for it to be detected

If a controller is already plugged in the target slot, the application exits without waiting.


## How it works

Virtual controllers are created to fill all available slots, except the requested one.
Therefore, when the real controller is plugged in, it can only get the right slot.

Virtual controllers are created using [ViGEmClient](https://github.com/nefarius/ViGEmClient).
They are all destroyed when the application exits.


## How to build

* MSYS2/mingw64 environement: run `make`
* MSVC tools: run `cl /O1 /std:c++20 /EHs /I ViGEmClient/include /Fegamepad-slotter.exe main.cpp ViGEmClient/src/ViGEmClient.cpp xinput.lib setupapi.lib`

C++20 support is required.

