#include "../src/input_capture.h"
#include <cstdio>
#include <string>
int main(){std::string result;const bool ok=hive::InputCapture::worker_self_test(result);std::printf("%s %s\n",ok?"PASS":"FAIL",result.c_str());return ok?0:1;}
