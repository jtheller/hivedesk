#include <cstdio>
#include <string>
namespace hive {bool media_async_decode_self_test(std::string&);}
int main(){std::string report;const bool passed=hive::media_async_decode_self_test(report);std::puts(report.c_str());return passed?0:1;}
