#pragma once
// One Kordlet card's or session's conversation, read from the collector (agent-chat/v1).
// Chat exists only per card/session; this view never sends, steers or approves anything.
#include <ArduinoJson.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
namespace agentchat {
constexpr uint8_t MAX_MESSAGES=8;
constexpr size_t MAX_JSON=16*1024,MAX_TEXT_BYTES=480*4;
struct Message {char role[12]{},text[MAX_TEXT_BYTES+1]{};double at=NAN;};
struct Model {
  bool valid=false,card=true,truncated=false;
  char system[65]{},id[65]{},title[97*4]{};
  double sampled=NAN;uint32_t total=0;uint8_t count=0;Message messages[MAX_MESSAGES];
};
struct State {
  bool active=false,loading=false,connected=false;uint16_t status=0;
  uint32_t revision=0,generation=0;char message[97]{"Open a card or session to read its chat."};
};
inline bool validId(const char*id){
  if(!id||!id[0]||strlen(id)>64)return false;
  for(size_t i=0;id[i];i++){char c=id[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||(i&&(c==':'||c=='.'||c=='_'||c=='-'))))return false;}
  return true;
}
// Ids match the identity pattern, so they need no escaping beyond ':' which is query-safe.
inline bool requestPath(char*out,size_t cap,const char*system,bool card,const char*id){
  if(!out||!cap||!validId(system)||!validId(id))return false;
  int n=snprintf(out,cap,"/api/v1/agent-operations/chat?system_id=%s&%s=%s",system,card?"card_id":"session_id",id);
  return n>0&&size_t(n)<cap;
}
template<size_t N>inline bool copyText(char(&out)[N],JsonVariantConst v,bool exact){
  if(!v.is<const char*>())return false;const char*input=v.as<const char*>();size_t length=strlen(input);
  if(length>=N){if(exact)return false;length=N-1;while(length&&(static_cast<unsigned char>(input[length])&0xc0)==0x80)--length;}
  memcpy(out,input,length);out[length]=0;return true;
}
inline double number(JsonVariantConst v){return v.is<bool>()||v.is<const char*>()?NAN:v.is<double>()?v.as<double>():NAN;}
// Accepts only the page that was asked for; any other shape leaves the model invalid.
inline bool parse(JsonDocument&doc,Model&out,const char*system,bool card,const char*id){
  new(&out)Model{};
  if(strcmp(doc["schema_version"]|"","agent-chat/v1")||strcmp(doc["system_id"]|"",system)||strcmp(doc["id"]|"",id)||strcmp(doc["scope"]|"",card?"card":"session"))return false;
  if(!doc["messages"].is<JsonArrayConst>()||doc["messages"].size()>MAX_MESSAGES||!doc["total"].is<uint32_t>()||!doc["truncated"].is<bool>())return false;
  strlcpy(out.system,system,sizeof(out.system));strlcpy(out.id,id,sizeof(out.id));out.card=card;
  if(!copyText(out.title,doc["title"],false))return false;
  out.sampled=number(doc["sampled_at"]);out.total=doc["total"].as<uint32_t>();out.truncated=doc["truncated"]==true;
  for(JsonObjectConst m:doc["messages"].as<JsonArrayConst>()){
    auto&r=out.messages[out.count];const char*role=m["role"]|"";
    if(strcmp(role,"user")&&strcmp(role,"assistant")&&strcmp(role,"system"))return false;
    strlcpy(r.role,role,sizeof(r.role));if(!copyText(r.text,m["text"],true))return false;
    r.at=number(m["at"]);if(isfinite(r.at)&&r.at<0)return false;out.count++;
  }
  if(out.total<out.count)return false;
  out.valid=true;return true;
}
}
#ifdef HOMELAB_NATIVE_PREVIEW
inline agentchat::Model&agentChatTransportModel(){static agentchat::Model value;return value;}
inline agentchat::State&agentChatTransportState(){static agentchat::State value;return value;}
inline bool readAgentChat(agentchat::Model&out,agentchat::State&state){out=agentChatTransportModel();state=agentChatTransportState();return true;}
#ifdef __EMSCRIPTEN__
void requestAgentChatTransport(const char*system,bool card,const char*id,uint32_t generation);
#else
inline void requestAgentChatTransport(const char*,bool,const char*,uint32_t){}
#endif
inline bool requestAgentChat(const char*system,bool card,const char*id){
  if(!agentchat::validId(system)||!agentchat::validId(id))return false;
  auto&state=agentChatTransportState();auto&model=agentChatTransportModel();
  bool same=model.valid&&model.card==card&&!strcmp(model.system,system)&&!strcmp(model.id,id);
  if(!same)new(&model)agentchat::Model{};
  state.active=true;state.loading=!same;state.generation++;state.revision++;strlcpy(state.message,"Loading conversation...",sizeof(state.message));
  requestAgentChatTransport(system,card,id,state.generation);return true;
}
inline void stopAgentChat(){auto&state=agentChatTransportState();state.active=false;state.loading=false;state.generation++;state.revision++;new(&agentChatTransportModel())agentchat::Model{};}
// Synthetic/test transport: a response body (or an HTTP status without one) for the current request.
inline bool ingestAgentChat(const char*json,uint16_t status,uint32_t generation,const char*system,bool card,const char*id){
  auto&state=agentChatTransportState();if(!state.active||generation!=state.generation)return false;
  state.loading=false;state.status=status;state.revision++;
  if(status!=200||!json){state.connected=status!=0;strlcpy(state.message,status==404?"Conversation not loaded yet. It appears after the next Kordlet poll.":status==401||status==403?"Collector display key rejected for chat.":status==503?"No Kordlet source is configured in the collector.":"Chat unavailable.",sizeof(state.message));return false;}
  JsonDocument doc;if(strlen(json)>agentchat::MAX_JSON||deserializeJson(doc,json,DeserializationOption::NestingLimit(8))||!agentchat::parse(doc,agentChatTransportModel(),system,card,id)){strlcpy(state.message,"Unsupported chat response.",sizeof(state.message));return false;}
  state.connected=true;state.message[0]=0;return true;
}
#else
bool requestAgentChat(const char*system,bool card,const char*id);
void stopAgentChat();
bool readAgentChat(agentchat::Model&out,agentchat::State&state);
#endif
