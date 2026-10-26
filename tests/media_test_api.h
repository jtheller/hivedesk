#pragma once
#include <string>
namespace hive {
bool media_auto_self_test(std::string& report);
// Desktop capture is intentionally separate from the headless codec self-test.
bool media_capture_self_test(std::string& report);
bool media_render_self_test(std::string& report);
bool media_throughput_self_test(std::string& report);
bool media_pipeline_self_test(std::string& report);
bool media_auto_quality_self_test(std::string& report);
bool media_motion_self_test(std::string& report);
bool media_color_self_test(std::string& report);
}
