#include "hivedesk/cursor_composition.h"
#include <cstdio>
#include <stdexcept>

int main(){try{
    using hive::CursorComposition;
    auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    hive::CaptureCursorComposition capture;
    require(capture.picture()==CursorComposition::Unknown&&!capture.needs_copy(),"Unknown capture ownership invented a pointer mode");
    capture.observe(0,false);
    require(capture.observed()==CursorComposition::Unknown,"Zero mouse timestamp applied an uninitialized visibility field");
    capture.copied();
    capture.observe(10,true);
    require(capture.needs_copy()&&capture.picture()==CursorComposition::Unknown,"First valid pointer metadata did not request an ownership copy");
    // Simulate copy failure by withholding copied(): later desktop-only
    // metadata cannot commit or cancel the still-unapplied ownership change.
    capture.observe(0,false);
    require(capture.needs_copy()&&capture.observed()==CursorComposition::Separate&&capture.picture()==CursorComposition::Unknown,
        "Failed copy changed cached ownership or zero metadata lost a pending transition");
    capture.copied();
    require(capture.picture()==CursorComposition::Separate&&!capture.needs_copy(),"Successful image copy did not commit ownership");
    for(int64_t tick=11;tick<1011;++tick){
        capture.observe(tick,true);
        require(!capture.needs_copy(),"Separate cursor motion consumed a video copy");
        capture.observe(0,false);
        require(capture.observed()==CursorComposition::Separate,"Desktop-only acquisition changed separate ownership");
    }
    capture.observe(1011,false);
    require(capture.needs_copy()&&capture.picture()==CursorComposition::Separate,"Embedded cursor transition reused a separate-cursor image");
    capture.copied();
    require(capture.picture()==CursorComposition::NoSeparate&&!capture.needs_copy(),"Embedded cursor copy was not retained");
    capture.observe(0,true);
    require(capture.picture()==CursorComposition::NoSeparate&&!capture.needs_copy(),"Invalid metadata removed embedded cursor ownership");
    capture.observe(1012,true);
    require(capture.needs_copy(),"Return to hardware cursor did not request a fresh image");
    capture.copied();
    require(capture.picture()==CursorComposition::Separate,"Hardware cursor return did not commit");
    capture.observe(1013,false);
    require(capture.needs_copy(),"Second embedded transition was missed");
    // A transition reverted before any image was copied must not label the
    // cached image with an ownership mode it never contained.
    capture.observe(1014,true);
    require(!capture.needs_copy()&&capture.picture()==CursorComposition::Separate,"Uncopied transient state changed cached image metadata");

    hive::PresentedCursorComposition presented;
    presented.reset(41);
    require(presented.visible(true,false)&&!presented.visible(false,false)&&!presented.visible(true,true),"Legacy visibility/relative behavior changed");
    require(!presented.present(40,200,CursorComposition::NoSeparate)&&presented.visible(true,false),"Stale session hid the current cursor");
    require(presented.present(41,0,CursorComposition::NoSeparate)&&!presented.visible(true,false),"Initial sequence zero did not hide a duplicated cursor");
    require(!presented.present(41,0,CursorComposition::Separate)&&!presented.visible(true,false),"Duplicate sequence changed ownership");
    require(presented.present(41,3,CursorComposition::Separate)&&presented.visible(true,false),"Presented hardware cursor did not reappear");
    require(!presented.present(41,2,CursorComposition::NoSeparate)&&presented.visible(true,false),"Older presented callback changed ownership");
    require(!presented.present(41,4,static_cast<CursorComposition>(3))&&presented.visible(true,false),"Invalid ownership changed presentation state");
    require(presented.present(41,4,CursorComposition::NoSeparate)&&!presented.visible(true,false),"Invalid callback consumed its sequence");
    require(presented.present(41,5,CursorComposition::Unknown)&&presented.visible(true,false),"Legacy frame did not preserve native cursor fallback");
    require(!presented.visible(true,true)&&!presented.visible(false,false),"Frame metadata overrode relative mode or host cursor hiding");
    presented.reset(42);
    require(presented.visible(true,false)&&!presented.present(41,999,CursorComposition::NoSeparate),"Reconnect retained old ownership or accepted old session");
    require(presented.present(42,0,CursorComposition::NoSeparate)&&!presented.visible(true,false),"Reconnect did not accept a fresh sequence zero");
    presented.reset();
    require(presented.present(0,0,CursorComposition::Unknown)&&presented.visible(true,false),"Legacy session zero lost cursor visibility");
    std::puts("PASS cursor composition: valid DXGI metadata, ownership-only copy, failed-copy retry, cached-image ownership, presented session/sequence guards, legacy visibility and relative mode.");return 0;
}catch(const std::exception& e){std::printf("FAIL cursor composition: %s\n",e.what());return 1;}}
