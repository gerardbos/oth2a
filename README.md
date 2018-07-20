# ESP32 OTA over h2

## Dependencies
- [ESP-IDF](https://github.com/espressif/esp-idf)
- [ESP32log](https://github.com/gerardbos/esp32log)
- [h2clientserver](https://github.com/gerardbos/h2clientserver)

### Features
- Upgrade firmware using a download from an HTTP/2 server
- Check if new firmware is available

## Usage and setup
First of all, the software version needs to live somewhere in the software. The OTA component
relies for that part on a external definition:

```c
extern const char * VERSION;
```

Second, a server URL has to be configured that is used to check the latest available software
version and to download the firmware blobs. This URL is defined in `CONFIG_OTH2A_BASE_URL`.
The firmware blob naming is constructed using `CONFIG_OTH2A_FIRMWARE_PREAMBLE` and
concattenated with `_{VERSION}.bin.

Finally a server URL
Setup a server with the following file structure:
```
.
├── latest
├── firmware_0.0.1.bin
└── firmware_0.0.2.bin
```
wheras the `latest` file contains the current version number (that shall be downloaded)
by the device and installed.

### Example

```c
#include "oth2a.h"


// in your main content
void main(){
	oth2a_initialize();

	// check if new firmware is available,
	// if so, update device and reset it.
	oth2a_handle();
}
```

