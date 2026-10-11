#include "FacialAnimationBridge.h"
#include <cstdlib>
#include <iostream>
namespace {
#define Check(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(0)
BFA_EventCallback callback=nullptr;
BFA_Line prepared{};
BFA_Emotion mood{};
uint64_t handles=41;
int starts=0, clocks=0, stops=0, ends=0;
bool reject=false;
void Event(uint32_t event,uint32_t reason,uint64_t handle) {
 BFA_Event ev{BFA_API_VERSION,sizeof(BFA_Event),event,reason,handle,
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
   Check(line->utterance_id);
   prepared=*line; mood=line->emotion?*line->emotion:BFA_Emotion{};
   prepared.emotion=line->emotion?&mood:nullptr;
   *result=reject?BFA_kResultInvalidArgument:BFA_kResultPending; return reject?0:++handles;
 };
 api.Start=[](uint64_t h,double)->BFA_Result {Check(h==handles);++starts;return BFA_kResultPending;};
 api.Pause=[](uint64_t)->BFA_Result {++clocks;return BFA_kResultOK;};
 api.Seek=[](uint64_t,double)->BFA_Result {++clocks;return BFA_kResultOK;};
 api.Stop=[](uint64_t,uint32_t)->BFA_Result {++stops;return BFA_kResultOK;};
 api.EndUtterance=[](uint64_t id,uint32_t)->BFA_Result {
   Check(id==prepared.utterance_id); ++ends; return BFA_kResultPending;
 };
 return api;
}
}
int main() {
 using namespace FacialAnimationBridge;
 auto api=MakeApi(); const char wav[]="RIFF0000WAVE";
 // A reply: its sentences share one utterance, and the face stays BFA's between them.
 std::shared_ptr<FaceToken> reply;
 {
  Line first(&api,0x14); reply=first.GetToken();
  Check(first.Prepare(wav,sizeof wav,"hello","DialogueHappy")==BFA_kResultPending && prepared.segment==0);
  Check(prepared.emotion && mood.channels[BFA_kJoy]==1 && reply->CanWrite());
  Check(first.Start(0)==BFA_kResultPending && reply->CanWrite()); // clock != animated
  Event(BFA_kStarted,0,42); Check(!reply->CanWrite() && !CanWriteActor(0x14));
  Check(first.Pause()==BFA_kResultOK && first.Seek(.2)==BFA_kResultOK);
  Check(first.Stop(BFA_kCompleted)==BFA_kResultOK && stops==1 && ends==0 && !CanWriteActor(0x14)); // the reply goes on
  Check(first.Seek(.3)==BFA_kResultOK && clocks==2); // stopped: no more clock calls
 }
 Check(stops==1 && !CanWriteActor(0x14));
 const auto utterance=prepared.utterance_id;
 {
  Line second(&api,0x14); Check(second.GetToken()==reply);
  Check(second.Prepare(wav,sizeof wav,"again","")==BFA_kResultPending && prepared.utterance_id==utterance && prepared.segment==1);
  Check(second.Start(0)==BFA_kResultPending); Event(BFA_kStarted,0,43); Check(!CanWriteActor(0x14));
  Event(BFA_kFailed,BFA_kResultInferenceFailed,43);
  Check(CanWriteActor(0x14) && second.LastResult()==BFA_kResultInferenceFailed); // fallback for this sentence
  int before=starts; Check(second.Start(0)==BFA_kResultInferenceFailed && starts==before);
  Event(BFA_kStarted,0,43); Check(CanWriteActor(0x14)); // late callback ignored
  Line third(&api,0x14); Check(third.GetToken()==reply);
  Check(third.Prepare(wav,sizeof wav,"","MoodNeutral")==BFA_kResultPending && prepared.segment==2 && mood.channels[BFA_kJoy]==0);
  Check(third.Start(0)==BFA_kResultPending); Event(BFA_kStarted,0,44); Check(!CanWriteActor(0x14));
 } // the sentences are stopped, the reply still open
 Check(stops==2 && ends==0 && !CanWriteActor(0x14));
 EndReply(0x14,BFA_kCompleted); Check(ends==1 && !CanWriteActor(0x14)); // BFA fades the face back first
 Event(BFA_kEnded,BFA_kCompleted,42); Check(CanWriteActor(0x14));
 Event(BFA_kEnded,BFA_kCompleted,44); Check(CanWriteActor(0x14));
 EndReply(0x14,BFA_kCompleted); Check(ends==1); // nothing open
 // The next sentence opens a new reply.
 {
  Line line(&api,0x14); Check(line.GetToken()!=reply);
  Check(line.Prepare(wav,sizeof wav,"","")==BFA_kResultPending && prepared.utterance_id!=utterance && prepared.segment==0);
 }
 EndReply(0x14,BFA_kAborted); Check(ends==2);
 Event(BFA_kEnded,BFA_kAborted,45);
 // A reply with no new sentence for StaleSeconds is over when the next one starts.
 { Line a(&api,0x14); Check(a.Prepare(wav,sizeof wav,"","")==BFA_kResultPending); }
 StaleSeconds()=0;
 { Line b(&api,0x14); Check(ends==3); Check(b.Prepare(wav,sizeof wav,"","")==BFA_kResultPending && prepared.segment==0); }
 StaleSeconds()=10; EndReply(0x14,BFA_kCompleted); Check(ends==4);
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
 std::cout << "Event bridge passed: one reply per response, pending fallback, animation ownership, terminal errors, reply cleanup\n";
}
