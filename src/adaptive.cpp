#include "hivedesk/adaptive.h"
#include <windows.h>
#include <stdexcept>
namespace hive {
uint32_t primary_refresh_millihz() {
    MONITORINFOEXW monitor{};monitor.cbSize=sizeof(monitor);
    if(!GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&monitor))throw std::runtime_error("Read primary monitor");
    for(int attempt=0;attempt<3;++attempt) {
        UINT32 paths=0,modes=0;
        if(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&paths,&modes)!=ERROR_SUCCESS)break;
        std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
        const auto result=QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&paths,p.data(),&modes,m.data(),nullptr);
        if(result==ERROR_INSUFFICIENT_BUFFER)continue;if(result!=ERROR_SUCCESS)break;
        for(UINT32 i=0;i<paths;++i) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};name.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            name.header.size=sizeof(name);name.header.adapterId=p[i].sourceInfo.adapterId;name.header.id=p[i].sourceInfo.id;
            if(DisplayConfigGetDeviceInfo(&name.header)!=ERROR_SUCCESS||wcscmp(name.viewGdiDeviceName,monitor.szDevice))continue;
            auto rate=p[i].targetInfo.refreshRate;const auto index=p[i].targetInfo.modeInfoIdx;
            if(index<modes&&m[index].infoType==DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)rate=m[index].targetMode.targetVideoSignalInfo.vSyncFreq;
            if(rate.Denominator){const auto value=uint64_t(rate.Numerator)*1000/rate.Denominator;if(value>=1000&&value<=2000000)return uint32_t(value);}
        }
    }
    DEVMODEW mode{};mode.dmSize=sizeof(mode);
    if(EnumDisplaySettingsW(monitor.szDevice,ENUM_CURRENT_SETTINGS,&mode)&&mode.dmDisplayFrequency>1&&mode.dmDisplayFrequency<=2000)return mode.dmDisplayFrequency*1000;
    throw std::runtime_error("Could not read primary display refresh rate");
}
bool adaptive_self_test(std::string& report) {
    auto require=[](bool pass,const char* text){if(!pass)throw std::runtime_error(text);};
    try {
        AdaptivePacer a(144000,59940),b(59940,240000);
        require(a.target()==59940&&b.target()==59940,"Fractional min-refresh negotiation");
        AdaptivePacer decoder(240000,144000);decoder.observe(1000000,1000,12000,0,false,false);
        require(decoder.rate()==75000&&(decoder.pressure()&PressureDecode),"Decoder capacity did not reduce source pacing");
        for(uint64_t now=2000000;now<8000000;now+=1000000)decoder.observe(now,1000,12000,0,false,false);
        require(decoder.rate()==75000,"Stable capacity caused oscillation");
        for(uint64_t now=8000000;now<30000000;now+=1000000)decoder.observe(now,1000,1000,0,false,false);
        require(decoder.rate()==144000,"Recovery did not reach the display target");
        AdaptivePacer spike(100000,144000);
        for(uint64_t now=1000000;now<=2400000;now+=10000)spike.observe(now,52863,1600,0,false,false,1000000);
        require(spike.rate()==100000,"Repeated observation of one encoder spike counted as sustained pressure");
        for(uint64_t now=2500000;now<2800000;now+=50000)spike.observe(now,52863,1600,0,false,false,now);
        spike.observe(2800000,4700,1600,0,false,false,2800000);
        require(spike.rate()==100000,"Brief fresh encoder spike collapsed the cap");
        AdaptivePacer encoder(99946,144000);
        uint32_t previous=encoder.rate();uint64_t last_drop=0;
        for(uint64_t now=1000000;now<=6000000;now+=50000) {
            encoder.observe(now,52863,1600,0,false,false,now);
            if(now<1500000)require(encoder.rate()==encoder.target(),"Encoder decrease did not wait for sustained samples");
            if(encoder.rate()!=previous) {
                require(encoder.rate()>=previous*80/100,"Encoder-only pressure cut more than twenty percent");
                require(!last_drop||now-last_drop>=500000,"Encoder-only decreases occurred more than twice per second");
                previous=encoder.rate();last_drop=now;
            }
        }
        require(encoder.rate()==900000000ull/52863&&(encoder.pressure()&PressureEncode),"Sustained encoder overload did not converge to measured capacity");
        // Reproduce the observed 52.863 ms -> 4.7 ms recovery envelope. The
        // old 5%/second rule keeps this below 40 FPS for more than 15 seconds.
        for(uint64_t now=6050000;now<=12000000;now+=50000) {
            encoder.observe(now,4700,1600,0,false,false,now);
            require(encoder.rate()<=99946,"Recovery exceeded fractional physical display target");
            if(now==8500000)require(encoder.rate()>40000,"Healthy encoding remained below forty FPS for multiple seconds");
        }
        require(encoder.rate()==99946&&!encoder.pressure(),"Healthy encoder did not regain full rate within six seconds");
        AdaptivePacer stale(100000,144000);
        for(uint64_t now=1000000;now<=6000000;now+=50000)stale.observe(now,52863,1600,0,false,false,now);
        require(stale.rate()<20000,"Stale sample test did not start with reduced cap");
        for(uint64_t now=6050000;now<=14000000;now+=50000)stale.observe(now,52863,1600,0,false,false,6000000);
        require(stale.rate()==100000,"Old encoder EMA kept an idle/resumed stream permanently capped");
        AdaptivePacer network(144000,240000);network.observe(1000000,1000,1000,0,true,false);
        require(network.rate()==115200&&(network.pressure()&PressureNetwork),"Network congestion did not reduce pacing");
        SendBackpressure send;
        require(!send.observe(1000000,false)&&!send.observe(1010000,false)&&send.observe(1021000,false),"Sustained send wait was not detected");
        require(!send.observe(1100000,true),"Cleared send wait still reported network pressure");
        require(!send.observe(1200000,false)&&!send.observe(1201000,true),"Separate brief waits accumulated pressure");
        const auto reduced=network.rate();network.observe(1100000,1000,1000,0,true,false);require(network.rate()==reduced,"Controller adjusted too frequently");
        AdaptivePacer queue(100000,144000);queue.observe(1000000,1000,1000,21000,false,false);
        require(queue.rate()==80000&&(queue.pressure()&PressureDecode),"Receiver backlog did not reduce pacing immediately");
        queue.observe(1250000,1000,1000,21000,false,false);require(queue.rate()==64000,"Persistent backlog no longer reduced pacing every quarter second");
        for(uint64_t now=1300000;now<3250000;now+=50000)queue.observe(now,1000,1000,0,false,false);
        require(queue.rate()==64000,"Receiver overload recovery skipped its two-second quiet period");
        queue.observe(3250000,1000,1000,0,false,false);require(queue.rate()>64000,"Receiver recovery never probed after pressure cleared");
        AdaptivePacer combined(100000,144000);combined.observe(1000000,52863,12000,0,false,false,1000000);
        require(combined.rate()==75000&&(combined.pressure()&PressureDecode),"Encoder debounce delayed an independently overloaded decoder");
        SlowIndicator warning;StreamHealth h{1,144000,60000,PressureDecode};
        require(!warning.update(1,h)&&!warning.update(900,h)&&warning.update(1100,h),"Slow indicator onset debounce");
        h.rate_millihz=144000;require(warning.update(1200,h)&&warning.update(4000,h)&&!warning.update(4300,h),"Slow indicator recovery debounce");
        warning.reset();h.rate_millihz=h.target_millihz;require(!warning.update(10000,h),"Idle desktop must not indicate slowdown");
        report="PASS: fractional min-refresh target, fresh-sample encoder hysteresis, bounded rapid recovery, stale-sample expiry, immediate decoder/network protection and idle-safe slowdown indicator.";return true;
    }catch(const std::exception& e){report=std::string("FAIL: ")+e.what();return false;}
}
}
