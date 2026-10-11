#include "FacialAnimationBridge.h"
#include <cstdlib>
#include <iostream>
namespace {
#define Check(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(0)
BFA_EventCallback callback=nullptr;
BFA_Line prepared{};
BFA_Emotion mood{};
int starts=0, clocks=0, ends=0, moods=0;
bool reject=false;
void Event(uint32_t event,uint32_t reason=BFA_ResultOK) {
 BFA_Event ev{BFA_API_VERSION,sizeof(BFA_Event),event,reason,42,
              prepared.utterance_id,0,prepared.speaker_form_id};
 callback(&ev,nullptr);
}
BFA_Interface MakeApi() {
 BFA_Interface api{}; api.version=BFA_API_VERSION; api.size=sizeof api;
 api.RegisterListener=[](BFA_EventCallback cb,void*,BFA_Result* result)->uint64_t {
   callback=cb; *result=BFA_ResultOK; return 1;
 };
 api.UnregisterListener=[](uint64_t)->BFA_Result {return BFA_ResultOK;};
 api.Prepare=[](const BFA_Line* line,BFA_Result* result)->uint64_t {
   prepared=*line;
   *result=reject?BFA_ResultInvalidArgument:BFA_ResultPending; return reject?0:42;
 };
 api.Start=[](uint64_t h,double)->BFA_Result {Check(h==42);++starts;return BFA_ResultPending;};
 api.Pause=[](uint64_t)->BFA_Result {++clocks;return BFA_ResultOK;};
 api.Resume=[](uint64_t,double)->BFA_Result {++clocks;return BFA_ResultOK;};
 api.Seek=[](uint64_t,double)->BFA_Result {++clocks;return BFA_ResultOK;};
 api.Stop=[](uint64_t,uint32_t)->BFA_Result {return BFA_ResultOK;};
 api.EndUtterance=[](uint64_t id,uint32_t)->BFA_Result {
   Check(id==prepared.utterance_id); ++ends; return BFA_ResultPending;
 };
 api.SetMood=[](uint32_t,const BFA_Emotion* value,float fade)->BFA_Result {
   Check(fade==0); mood=*value; ++moods;return BFA_ResultOK;
 };
 return api;
}
}
int main() {
 using namespace FacialAnimationBridge;
 auto api=MakeApi(); const char wav[]="RIFF0000WAVE";
 {
  Line line(&api,0x14);
  auto token=line.GetToken();
  Check(line.Prepare(wav,sizeof wav,"hello","DialogueHappy")==BFA_ResultPending);
  Check(mood.channels[BFA_kJoy]==1 && token->CanWrite());
  Check(line.Start(0)==BFA_ResultPending && token->CanWrite()); // clock != animated
  Event(BFA_kStarted); Check(!token->CanWrite() && !CanWriteActor(0x14));
  Check(line.Pause()==BFA_ResultOK && line.Resume(.2)==BFA_ResultOK);
  Check(line.Seek(.3)==BFA_ResultOK);
  Event(BFA_kFailed,BFA_ResultInferenceFailed);
  Check(token->CanWrite() && line.LastResult()==BFA_ResultInferenceFailed);
  int before=starts; Check(line.Start(0)==BFA_ResultInferenceFailed && starts==before);
  Event(BFA_kStarted); Check(token->CanWrite()); // late callback ignored
  line.Stop(BFA_kCompleted); Check(!token->CanWrite() && ends==1);
 }
 Check(ends==1);
 {
  Line line(nullptr,0x14);
  Check(line.Prepare(wav,sizeof wav,"","")==BFA_ResultUnavailable);
  Check(line.Start(0)==BFA_ResultUnavailable && line.GetToken()->CanWrite());
 }
 reject=true;
 {
  Line line(&api,0x14);
  Check(line.Prepare(wav,sizeof wav,"","")==BFA_ResultInvalidArgument);
  Check(line.Start(0)==BFA_ResultInvalidArgument && line.GetToken()->CanWrite());
 }
 reject=false;
 std::shared_ptr<FaceToken> queued;
 {
  Line line(&api,0x14); queued=line.GetToken();
  Check(line.Prepare(wav,sizeof wav,"","MoodNeutral")==BFA_ResultPending);
  Check(mood.channels[BFA_kJoy]==0);
  Event(BFA_kStarted); Check(!queued->CanWrite());
  Check(line.Stop(BFA_kSkipped)==BFA_ResultPending);
  Check(!CanWriteActor(0x14)); // cleanup has not finished yet
 }
 Check(!queued->CanWrite());
 Event(BFA_kEnded,BFA_kSkipped); Check(CanWriteActor(0x14));
 Check(ends==2);
 std::cout << "Event bridge passed: pending fallback, animation ownership, terminal errors, reply cleanup\n";
}
