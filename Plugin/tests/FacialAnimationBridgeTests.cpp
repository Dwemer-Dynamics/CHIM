#include "FacialAnimationBridge.h"
#include <cstdlib>
#include <iostream>
namespace {
#define Check(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(0)
BFA_EventCallback callback=nullptr;
BFA_Line prepared{};
BFA_Emotion mood{};
int starts=0, clocks=0, ends=0;
bool reject=false;
void Event(uint32_t event,uint32_t reason=BFA_kResultOK) {
 BFA_Event ev{BFA_API_VERSION,sizeof(BFA_Event),event,reason,42,
              prepared.utterance_id,0,prepared.speaker_form_id};
 callback(&ev,nullptr);
}
BFA_Interface MakeApi() {
 BFA_Interface api{}; api.version=BFA_API_VERSION; api.size=sizeof api;
 api.RegisterListener=[](BFA_EventCallback cb,void*,BFA_Result* result)->uint64_t {
   callback=cb; *result=BFA_kResultOK; return 1;
 };
 api.UnregisterListener=[](uint64_t)->BFA_Result {return BFA_kResultOK;};
 api.NewUtteranceId=[]()->uint64_t { static uint64_t next=0; return ++next; };
 api.Prepare=[](const BFA_Line* line,BFA_Result* result)->uint64_t {
   Check(line->utterance_id && line->utterance_id!=prepared.utterance_id); // a fresh plugin id per reply
   prepared=*line; mood=line->emotion?*line->emotion:BFA_Emotion{};
   prepared.emotion=line->emotion?&mood:nullptr;
   *result=reject?BFA_kResultInvalidArgument:BFA_kResultPending; return reject?0:42;
 };
 api.Start=[](uint64_t h,double)->BFA_Result {Check(h==42);++starts;return BFA_kResultPending;};
 api.Pause=[](uint64_t)->BFA_Result {++clocks;return BFA_kResultOK;};
 api.Seek=[](uint64_t,double)->BFA_Result {++clocks;return BFA_kResultOK;};
 api.Stop=[](uint64_t,uint32_t)->BFA_Result {return BFA_kResultOK;};
 api.EndUtterance=[](uint64_t id,uint32_t)->BFA_Result {
   Check(id==prepared.utterance_id); ++ends; return BFA_kResultPending;
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
  Check(line.Prepare(wav,sizeof wav,"hello","DialogueHappy")==BFA_kResultPending);
  Check(prepared.emotion && mood.channels[BFA_kJoy]==1 && token->CanWrite());
  Check(line.Start(0)==BFA_kResultPending && token->CanWrite()); // clock != animated
  Event(BFA_kStarted); Check(!token->CanWrite() && !CanWriteActor(0x14));
  Check(line.Pause()==BFA_kResultOK && line.Seek(.2)==BFA_kResultOK);
  Check(line.Seek(.3)==BFA_kResultOK);
  Event(BFA_kFailed,BFA_kResultInferenceFailed);
  Check(token->CanWrite() && line.LastResult()==BFA_kResultInferenceFailed);
  int before=starts; Check(line.Start(0)==BFA_kResultInferenceFailed && starts==before);
  Event(BFA_kStarted); Check(token->CanWrite()); // late callback ignored
  line.Stop(BFA_kCompleted); Check(!token->CanWrite() && ends==1);
 }
 Check(ends==1);
 {
  Line line(nullptr,0x14);
  Check(line.Prepare(wav,sizeof wav,"","")==BFA_kResultUnavailable);
  Check(line.Start(0)==BFA_kResultUnavailable && line.GetToken()->CanWrite());
 }
 reject=true;
 {
  Line line(&api,0x14);
  Check(line.Prepare(wav,sizeof wav,"","")==BFA_kResultInvalidArgument);
  Check(line.Start(0)==BFA_kResultInvalidArgument && line.GetToken()->CanWrite());
 }
 reject=false;
 std::shared_ptr<FaceToken> queued;
 {
  Line line(&api,0x14); queued=line.GetToken();
  Check(line.Prepare(wav,sizeof wav,"","MoodNeutral")==BFA_kResultPending);
  Check(prepared.emotion && mood.channels[BFA_kJoy]==0);
  Event(BFA_kStarted); Check(!queued->CanWrite());
  Check(line.Stop(BFA_kSkipped)==BFA_kResultPending);
  Check(!CanWriteActor(0x14)); // cleanup has not finished yet
 }
 Check(!queued->CanWrite());
 Event(BFA_kEnded,BFA_kSkipped); Check(CanWriteActor(0x14));
 Check(ends==2);
 std::cout << "Event bridge passed: pending fallback, animation ownership, terminal errors, reply cleanup\n";
}
