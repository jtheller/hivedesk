#pragma once
#include "types.h"
#include <windows.h>
namespace hive {
Snapshot capture_snapshot(uint64_t request,bool native=false);
Thumbnail decode_snapshot(const Snapshot&);
bool read_cursor_shape(HCURSOR, CursorState&);
HCURSOR make_cursor(const CursorState&); // caller owns DestroyCursor
bool desktop_aux_self_test(std::string& report);
bool snapshot_capture_self_test(std::string& report);
}
