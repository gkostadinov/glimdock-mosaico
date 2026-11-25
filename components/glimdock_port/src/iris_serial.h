// SPDX-License-Identifier: Apache-2.0
// A serial line for Glimdock over ESP-Iris RPC. On the Mosaico, USB belongs to
// ESP-Iris, so the private reader's provisioning frames (written to a serial
// port on the desk boards) arrive here instead:
//   service 0x4744 ("GD") method 1: append the payload to the input
//   service 0x4744 method 2: return (and drop) pending output
// tools/private-reader-mosaico.py drives it from the Mac.
#pragma once

#include "Arduino.h"

class IrisSerial : public Stream {
public:
    int available() override;
    int read() override;
    int peek() override;
    size_t write(uint8_t value) override;
    using Print::write;
};

IrisSerial &glim_iris_serial();
void glim_iris_serial_register();
