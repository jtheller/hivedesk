#include "hivedesk/media.h"
#include "media_test_api.h"
#include <iostream>
#include <string_view>
int main(int argc, char** argv) {
    std::string report;
    const std::string_view test = argc > 1 ? argv[1] : "codec";
    const bool ok = test == "capture" ? hive::media_capture_self_test(report) :
        test == "render" ? hive::media_render_self_test(report) :
        test == "throughput" ? hive::media_throughput_self_test(report) : hive::media_self_test(report);
    std::cout << report << '\n'; return ok ? 0 : 1;
}
