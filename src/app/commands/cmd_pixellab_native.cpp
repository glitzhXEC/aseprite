// Experimental PixelLab native integration. Not yet a complete save workflow.
#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif
#include "app/commands/command.h"
#include "app/context.h"
#include "app/context_access.h"
#include "ui/alert.h"
#ifdef ENABLE_PIXELLAB_NATIVE
#include "app/doc.h"
#include "app/job.h"
#include "app/pixellab/client.h"
#include "vendor/json.hpp"
#include "doc/cel.h"
#include "doc/color.h"
#include "doc/image.h"
#include "doc/image_spec.h"
#include "doc/layer.h"
#include "doc/palette.h"
#include "doc/primitives.h"
#include "doc/sprite.h"
#include "doc/tag.h"
#include "doc/tags.h"
#include "render/render.h"
#include "ui/box.h"
#include "ui/button.h"
#include "ui/combobox.h"
#include "ui/entry.h"
#include "ui/label.h"
#include "ui/window.h"
#include "ui/paint_event.h"
#include "ui/size_hint_event.h"
#include "ui/separator.h"
#include "ui/timer.h"
#include "ui/scale.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace app { namespace {
using json=nlohmann::json;
static pixellab::Client client;
// Session-only originals. Never write snapshots or keys to disk.
struct Snapshot { json source; doc::ObjectId layer; doc::ObjectId skeleton; };
static std::map<doc::ObjectId,Snapshot> snapshots;
class RequestJob final : public Job {
public:
 explicit RequestJob(const json& j):Job("PixelLab",true), request(j.dump()) {}
 json result;
protected:
 void onJob() override {result=json::parse(client.run(request));}
private:
 std::string request;
};
static json run(const json& request){RequestJob job(request);job.startJob();job.waitJob();if(job.isCanceled())throw std::runtime_error("Request cancelled. Please refresh before retrying.");if(!job.result.value("ok",false))throw std::runtime_error(job.result.value("error",std::string("Request failed")));return job.result;}
static std::string lower(std::string s){for(char& c:s)c=(char)std::tolower((unsigned char)c);return s;}
static std::string displayAnimation(std::string name){auto n=lower(name);if(n.rfind("custom-",0)==0||n.rfind("custom_",0)==0)name=name.substr(7);return name;}
static std::string safeLabel(std::string s){for(char& c:s)if(c=='|'||c=='\n'||c=='\r')c=' ';if(s.size()>110)s.resize(110);return s;}
static int prompt(const std::string& title,const std::vector<std::string>& lines,const std::vector<std::string>& buttons){ui::Alert dialog;dialog.setTitle(title);for(const auto& line:lines)dialog.addLabel(line,ui::LEFT);for(const auto& button:buttons)dialog.addButton(button);return dialog.show();}
static void error(const std::exception& e){prompt("PixelLab error",{safeLabel(e.what())},{"&OK"});}
static int nibble(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;throw std::runtime_error("Invalid image data");}
static doc::ImageRef image(const json& data,int w,int h){
 auto hex=data.at("hex").get<std::string>(); if(data.at("w")!=w||data.at("h")!=h||hex.size()!=size_t(w)*h*8)throw std::runtime_error("Invalid image size");
 doc::ImageRef out(doc::Image::create(doc::IMAGE_RGB,w,h));size_t i=0;
 auto octet=[&](){int a=nibble(hex[i++]),b=nibble(hex[i++]);return (a<<4)|b;};
 for(int y=0;y<h;y++)for(int x=0;x<w;x++){int r=octet(),g=octet(),b=octet(),a=octet();doc::put_pixel(out.get(),x,y,doc::rgba(r,g,b,a));}return out;
}
static void openSprite(Context* ctx,const json& data,bool animation){
 int w=data.at("w").get<int>(),h=data.at("h").get<int>();if(w<1||h<1||w>4096||h>4096)throw std::runtime_error("Invalid image size");
 std::unique_ptr<doc::Sprite> spr(new doc::Sprite(doc::ImageSpec(doc::ColorMode::RGB,w,h),256));
 // Populate palette from the imported artwork instead of leaving it black.
 if(data.contains("palette")&&data["palette"].is_array()){
  int index=0;for(const auto& value:data["palette"]){if(index>=256||!value.is_string())break;auto c=value.get<std::string>();if(c.size()!=8)continue;int r=nibble(c[0])*16+nibble(c[1]),g=nibble(c[2])*16+nibble(c[3]),b=nibble(c[4])*16+nibble(c[5]),a=nibble(c[6])*16+nibble(c[7]);spr->palette(0)->setEntry(index++,doc::rgba(r,g,b,a));}
 }
 auto* layer=new doc::LayerImage(spr.get());layer->setName("Character");spr->root()->addLayer(layer);
 auto* bones=new doc::LayerImage(spr.get());bones->setName("Skeleton (reference)");bones->setVisible(false);spr->root()->addLayer(bones);
 int frame=0;std::vector<std::pair<std::string,std::pair<int,int>>> ranges;
 auto add=[&](const json& item,const std::string& mark){auto* cel=new doc::Cel(frame,image(item.at("image"),w,h));cel->data()->userData().setText(mark);layer->addCel(cel);if(item.contains("skeleton"))bones->addCel(new doc::Cel(frame,image(item.at("skeleton"),w,h)));frame++;};
 if(animation){for(const auto& clip:data.at("clips")){int first=frame;int i=0;for(const auto& f:clip.at("frames"))add(f,"pixellab:"+clip.at("direction").get<std::string>()+":"+std::to_string(i++));ranges.push_back({clip.at("direction").get<std::string>(),{first,frame-1}});}}
 else {for(const auto& f:data.at("frames"))add(f,"pixellab:"+f.at("direction").get<std::string>()+":0");}
 if(!frame)throw std::runtime_error("No frames available");spr->setTotalFrames(frame);spr->setDurationForAllFrames(100);
 if(animation&&ranges.size()>1)for(auto& r:ranges){auto* tag=new doc::Tag(r.second.first,r.second.second);tag->setName(r.first);spr->tags().add(tag);}
 // Keep exact asset IDs, but snapshots and binding remain session-only.
 json metadata={{"pixellab_native_wip",1},{"character_id",data.at("id")},{"type",animation?"animation":"rotations"}};
 if(animation)metadata["group_id"]=data.at("group_id");spr->userData().setText(metadata.dump());
 snapshots[spr->id()]={data,layer->id(),bones->id()};
 std::unique_ptr<Doc> document(new Doc(spr.get()));spr.release();document->setFilename(data.value("label",std::string("PixelLab")));document->setContext(ctx);document.release();
}
static std::string renderFrame(const doc::Sprite* sprite,int frame,bool animation){
 doc::ImageRef out(doc::Image::create(doc::IMAGE_RGB,sprite->width(),sprite->height()));
 doc::clear_image(out.get(),doc::rgba(0,0,0,0));render::Render renderer;renderer.renderSprite(out.get(),sprite,frame);
 static const char* digits="0123456789abcdef";std::string hex;hex.reserve(size_t(sprite->width())*sprite->height()*8);
 auto add=[&](unsigned x){hex.push_back(digits[x>>4]);hex.push_back(digits[x&15]);};
 for(int y=0;y<sprite->height();y++)for(int x=0;x<sprite->width();x++){auto c=doc::get_pixel(out.get(),x,y);int a=doc::rgba_geta(c);if(animation&&a>0&&a<255)throw std::runtime_error("GIF cannot preserve partial transparency. No changes sent.");add(a?doc::rgba_getr(c):0);add(a?doc::rgba_getg(c):0);add(a?doc::rgba_getb(c):0);add(a);}
 return hex;
}
static void saveSprite(Context* ctx){
 json request;std::string directionList,targetName;doc::ObjectId spriteId=doc::NullId;
 {
  ContextReader read(ctx);auto* spr=read.sprite();if(!spr)throw std::runtime_error("Open a PixelLab sprite first.");spriteId=spr->id();auto it=snapshots.find(spriteId);if(it==snapshots.end())throw std::runtime_error("Original snapshot not found. Reopen this asset from PixelLab.");
  auto& snap=it->second;auto& source=snap.source;int w=source.at("w"),h=source.at("h");if(spr->width()!=w||spr->height()!=h)throw std::runtime_error("Canvas size changed. No changes sent.");
  const doc::Layer* character=nullptr;const doc::Layer* skeleton=nullptr;for(const auto* layer:spr->root()->layers()){if(layer->id()==snap.layer)character=layer;if(layer->id()==snap.skeleton)skeleton=layer;}
  if(!character||!character->isImage()||!skeleton)throw std::runtime_error("Original layers missing. Reopen this asset.");
  if(skeleton->isVisible())throw std::runtime_error("Hide the Skeleton reference layer before saving.");
  bool anim=source.contains("clips");
  if(anim&&source.value("fallback_match",false))throw std::runtime_error("Animation was opened using an unverified export match. Saving is blocked to protect the original.");
  targetName=safeLabel(source.value("label",std::string("Character")))+" ["+source.at("id").get<std::string>().substr(0,8)+"]";if(anim)targetName+=" - "+safeLabel(displayAnimation(source.value("name",std::string("Animation"))));json items=json::array();int frame=0;
  auto verify=[&](const std::string& direction,int i){auto* cel=character->cel(frame);if(!cel||cel->data()->userData().text()!="pixellab:"+direction+":"+std::to_string(i))throw std::runtime_error("Original frames were moved, replaced, or deleted. Reopen the asset to avoid saving to the wrong direction.");};
  if(anim){for(const auto& clip:source.at("clips")){auto dir=clip.at("direction").get<std::string>();json frames=json::array(),original=json::array();bool changed=false;int n=0,delay=-1;
    for(const auto& f:clip.at("frames")){if(frame>=spr->totalFrames())throw std::runtime_error("Frame count changed. Reopen the asset.");verify(dir,n++);int duration=spr->frameDuration(frame);if(duration<10||duration%10|| (delay!=-1&&delay!=duration))throw std::runtime_error("GIF needs the same frame duration in 10ms units. No changes sent.");delay=duration;if(duration!=100)changed=true;auto now=renderFrame(spr,frame,true);auto old=f.at("image").at("hex").get<std::string>();if(now!=old)changed=true;frames.push_back(now);original.push_back(old);frame++;}
    if(changed){items.push_back({{"direction",dir},{"w",w},{"h",h},{"frames",frames},{"original_frames",original},{"delay_cs",delay/10}});if(!directionList.empty())directionList+=", ";directionList+=dir;}
   }}else{for(const auto& f:source.at("frames")){if(frame>=spr->totalFrames())throw std::runtime_error("Frame count changed. Reopen the asset.");auto dir=f.at("direction").get<std::string>();verify(dir,0);auto now=renderFrame(spr,frame,false);auto old=f.at("image").at("hex").get<std::string>();if(now!=old){items.push_back({{"direction",dir},{"w",w},{"h",h},{"hex",now},{"original_hex",old}});if(!directionList.empty())directionList+=", ";directionList+=dir;}frame++;}}
  if(frame!=spr->totalFrames())throw std::runtime_error("Frame count changed. Reopen the asset.");
  if(items.empty()){prompt("PixelLab",{"No pixel changes to save."},{"&OK"});return;}
  request={{"action",anim?"upload_animation":"upload"},{"character_id",source.at("id")},{"items",items}};if(anim)request["group_id"]=source.at("group_id");
 }
 // Explicit confirmation after checking all frame bindings, before cloud writes.
 if(prompt("Replace on PixelLab?",{"Experimental - test on a copy.","Target: "+targetName,"Directions: "+directionList,"Cloud save cannot be undone here.","Animations use uniform timeline timing (default 100ms)."},{"&Save","&Cancel"})!=1)return;
 snapshots.erase(spriteId); // Outcome can be uncertain even if the request fails.
 auto result=run(request);std::string msg="Saved: ";for(auto& d:result.at("saved"))msg+=d.get<std::string>()+" ";auto failed=result.at("failed");if(!failed.empty()){msg+="  Failed: ";for(auto& f:failed)msg+=f.value("direction",std::string("?"))+" ";}prompt("PixelLab result",{safeLabel(msg),"Refresh PixelLab and reopen before saving again."},{"&OK"});
}
// Preview paints a bounded number of screen pixels and never mutates source images.
class PixelLabPreview final : public ui::Widget {
public:
 PixelLabPreview():timer(100,this){setExpansive(true);timer.Tick.connect([this]{if(images.size()>1){frame=(frame+1)%images.size();invalidate();}});timer.start();}
 void setImages(const json& source){images.clear();frame=0;for(const auto& p:source){int w=p.value("w",0),h=p.value("h",0);if(w>0&&h>0&&w<=4096&&h<=4096)images.push_back(image(p,w,h));}invalidate();}
protected:
 void onSizeHint(ui::SizeHintEvent& e) override {e.setSizeHint(240*ui::guiscale(),80*ui::guiscale());}
 void onPaint(ui::PaintEvent& ev) override {
  auto* g=ev.graphics();auto rc=clientBounds();int tile=8*ui::guiscale();
  for(int y=0;y<rc.h;y+=tile)for(int x=0;x<rc.w;x+=tile)g->fillRect(((x/tile+y/tile)%2)?gfx::rgba(49,49,55):gfx::rgba(60,60,66),gfx::Rect(rc.x+x,rc.y+y,std::min(tile,rc.w-x),std::min(tile,rc.h-y)));
  if(images.empty()){g->drawAlignedUIText("Choose a character",gfx::rgba(170,170,180),gfx::ColorNone,rc,ui::CENTER|ui::MIDDLE);return;}
  auto im=images[frame];int iw=im->width(),ih=im->height();
  double scale=std::min(double(rc.w)/iw,double(rc.h)/ih);scale=std::min(scale,6.0*ui::guiscale());
  int dw=std::max(1,int(iw*scale)),dh=std::max(1,int(ih*scale));int ox=rc.x+(rc.w-dw)/2,oy=rc.y+(rc.h-dh)/2;
  // Composite each pixel against the checkerboard for accurate partial alpha.
  for(int y=0;y<dh;y++)for(int x=0;x<dw;x++){auto c=doc::get_pixel(im.get(),x*iw/dw,y*ih/dh);int a=doc::rgba_geta(c);if(!a)continue;int bg=(((ox+x-rc.x)/tile+(oy+y-rc.y)/tile)%2)?49:60;int r=(doc::rgba_getr(c)*a+bg*(255-a))/255, gg=(doc::rgba_getg(c)*a+bg*(255-a))/255,b=(doc::rgba_getb(c)*a+bg*(255-a))/255;g->fillRect(gfx::rgba(r,gg,b),gfx::Rect(ox+x,oy+y,1,1));}
 }
private:ui::Timer timer;std::vector<doc::ImageRef> images;size_t frame=0;
};
class Panel final : public ui::Window {
public:
 explicit Panel(Context* context,const std::string& connection):Window(WithTitleBar,"PixelLab Native (Experimental)"),ctx(context),debounce(300,this){
  auto* root=new ui::Box(ui::VERTICAL);addChild(root);
#ifdef PIXELLAB_FAKE_NETWORK_FOR_TESTS
  status=new ui::Label("SIMULATION ONLY - no cloud writes");
#else
  status=new ui::Label(connection.empty()?"Connected (environment key)":"Connected - "+safeLabel(connection));
#endif
  root->addChild(status);
  root->addChild(new ui::Separator("1. Character",ui::HORIZONTAL));
  auto* searchrow=new ui::Box(ui::HORIZONTAL);root->addChild(searchrow);searchrow->addChild(new ui::Label("Search"));search=new ui::Entry(200,"");search->setExpansive(true);searchrow->addChild(search);
  auto* charrow=new ui::Box(ui::HORIZONTAL);root->addChild(charrow);charrow->addChild(new ui::Label("Character"));chars=new ui::ComboBox;chars->setExpansive(true);charrow->addChild(chars);
  refresh=new ui::Button("Refresh list");root->addChild(refresh);
  root->addChild(new ui::Separator("2. Pick and preview",ui::HORIZONTAL));
  auto* typeRow=new ui::Box(ui::HORIZONTAL);root->addChild(typeRow);typeRow->addChild(new ui::Label("Open"));types=new ui::ComboBox;types->setExpansive(true);typeRow->addChild(types);
  auto* dirRow=new ui::Box(ui::HORIZONTAL);root->addChild(dirRow);dirRow->addChild(new ui::Label("Direction"));dirs=new ui::ComboBox;dirs->setExpansive(true);dirRow->addChild(dirs);
  preview=new PixelLabPreview;root->addChild(preview);info=new ui::Label("Choose a character");root->addChild(info);
  estimate=new ui::CheckBox("Add skeleton if missing (0.1 generation per frame)");root->addChild(estimate);
  open=new ui::Button("Open in Aseprite");root->addChild(open);
  root->addChild(new ui::Separator("3. Save back",ui::HORIZONTAL));
  auto* active=ctx->activeDocument();bool bound=active&&snapshots.count(active->sprite()->id());
  activeLabel=new ui::Label(bound?"Current sprite is linked to PixelLab.":"Open a PixelLab sprite to save it here.");root->addChild(activeLabel);
  auto* actions=new ui::Box(ui::HORIZONTAL|ui::HOMOGENEOUS);root->addChild(actions);save=new ui::Button("Save to PixelLab...");save->setEnabled(bound);actions->addChild(save);auto* undo=new ui::Button("Undo last save (unavailable)");undo->setEnabled(false);actions->addChild(undo);
  debounce.Tick.connect([this]{debounce.stop();filter();});search->Change.connect([this]{debounce.stop();debounce.start();});chars->Change.connect([this]{select();});types->Change.connect([this]{updateDirections();});dirs->Change.connect([this]{updatePreview();});refresh->Click.connect([this]{load(true);});open->Click.connect([this]{openSelected();});save->Click.connect([this]{try{saveSprite(ctx);}catch(const std::exception& e){error(e);}auto* active=ctx->activeDocument();bool linked=active&&snapshots.count(active->sprite()->id());save->setEnabled(linked);activeLabel->setText(linked?"Current sprite is linked to PixelLab.":"Reopen the PixelLab sprite before saving again.");});
  remapWindow();centerWindow();load(false);
 }
private:
 Context* ctx;ui::Timer debounce;ui::Entry* search;ui::Button *refresh,*open,*save;ui::CheckBox* estimate;ui::Label *status,*info,*activeLabel;PixelLabPreview* preview;ui::ComboBox *chars,*types,*dirs;json all=json::array(),filtered=json::array(),animations=json::array(),character;std::string selected;bool updating=false,busy=false;
 void reset(ui::ComboBox* box){box->deleteAllItems();box->getEntryWidget()->setText("");}
 void load(bool force){if(busy)return;try{busy=true;all=run({{"action","list"},{"force",force}}).at("characters");busy=false;selected.clear();filter();}catch(const std::exception& e){busy=false;error(e);}}
 void filter(){if(busy){debounce.start();return;}updating=true;auto q=lower(search->text());auto previous=selected;filtered=json::array();reset(chars);int chosen=-1;for(const auto& c:all){auto name=c.value("label",std::string("Untitled"));if(lower(name).find(q)==std::string::npos)continue;if(c.at("id").get<std::string>()==previous)chosen=(int)filtered.size();filtered.push_back(c);chars->addItem(safeLabel(name)+" ["+c.at("id").get<std::string>().substr(0,8)+"]");}if(!filtered.empty())chars->setSelectedItemIndex(chosen>=0?chosen:0);else chars->getEntryWidget()->setText("No matching characters");chars->setEnabled(!filtered.empty());updating=false;select();}
 void select(){if(updating||busy)return;auto n=chars->getSelectedItemIndex();std::string id=(n>=0&&n<int(filtered.size()))?filtered[n].at("id").get<std::string>():"";if(id==selected&&!id.empty())return;selected=id;updating=true;reset(types);reset(dirs);animations=json::array();character=json();preview->setImages(json::array());types->setEnabled(false);dirs->setEnabled(false);open->setEnabled(false);updating=false;
  if(id.empty()){info->setText("No matching characters");return;}
  try{busy=true;character=run({{"action","select"},{"character_id",id}});try{animations=run({{"action","animations"},{"character_id",id}}).at("animations");}catch(const std::exception& e){animations=json::array();error(e);}busy=false;updating=true;types->addItem("Rotations");for(const auto& a:animations)types->addItem(safeLabel(displayAnimation(a.value("name",std::string("Animation")))));types->setSelectedItemIndex(0);types->setEnabled(true);open->setEnabled(true);updating=false;updateDirections();}catch(const std::exception& e){busy=false;updating=false;selected.clear();info->setText("Could not load character; refresh to retry.");error(e);}}
 void updateDirections(){if(updating||busy)return;updating=true;reset(dirs);int n=types->getSelectedItemIndex();dirs->setEnabled(n>0);if(n>0&&n<=int(animations.size())){dirs->addItem("All directions");for(const auto& d:animations.at(n-1).at("directions"))dirs->addItem(d.get<std::string>());dirs->setSelectedItemIndex(0);}else dirs->getEntryWidget()->setText("All directions");updating=false;updatePreview();}
 void updatePreview(){if(updating||busy||selected.empty())return;try{int n=types->getSelectedItemIndex();if(n<=0){preview->setImages(character.at("previews"));info->setText(std::to_string(character.value("w",0))+" x "+std::to_string(character.value("h",0))+" px  |  skeleton: "+(character.value("has_skeleton",false)?"provided":"not provided"));}else{const auto& a=animations.at(n-1);int d=dirs->getSelectedItemIndex();auto dir=a.at("directions").at(d>0?d-1:0).get<std::string>();busy=true;auto result=run({{"action","anim_preview"},{"character_id",selected},{"group_id",a.at("group_id")},{"name",a.at("name")},{"direction",dir}});busy=false;preview->setImages(result.at("frames"));info->setText("Preview: "+dir+"  |  "+std::to_string(result.at("frames").size())+" frames  |  100 ms/frame");}}catch(const std::exception& e){busy=false;preview->setImages(json::array());info->setText("Preview unavailable; opening may still work.");error(e);}}
 void openSelected(){if(busy)return;try{if(selected.empty())throw std::runtime_error("Choose a character");int n=types->getSelectedItemIndex();if(n<0)throw std::runtime_error("Choose content to open");bool paid=estimate->isSelected();if(paid&&prompt("Estimate skeletons?",{"PixelLab may charge 0.1 generation for each frame missing a skeleton.","This can apply to every selected direction and frame.","Continue?"},{"&Continue","&Cancel"})!=1)return;busy=true;if(!n)openSprite(ctx,run({{"action","open_rotations"},{"character_id",selected},{"estimate",paid}}),false);else{const auto& a=animations.at(n-1);auto chosen=a.at("directions");auto d=dirs->getSelectedItemIndex();if(d>0)chosen=json::array({a.at("directions").at(d-1)});openSprite(ctx,run({{"action","open_animation"},{"character_id",selected},{"group_id",a.at("group_id")},{"name",a.at("name")},{"directions",chosen},{"estimate",paid}}),true);}busy=false;closeWindow(nullptr);}catch(const std::exception& e){busy=false;error(e);}}
};
} }
#endif
namespace app {
class PixelLabNativeCommand final : public Command {
public: PixelLabNativeCommand():Command("PixelLabNative"){}
protected:
 bool onEnabled(Context* ctx) override{return ctx->isUIAvailable();}
 void onExecute(Context* ctx) override {
#ifdef ENABLE_PIXELLAB_NATIVE
#ifdef PIXELLAB_FAKE_NETWORK_FOR_TESTS
  const char* key="synthetic";
#else
  const char* key=std::getenv("PIXELLAB_API_KEY");
#endif
  if(!key||!*key){prompt("PixelLab native development",{"Set PIXELLAB_API_KEY in the environment before starting Aseprite.","This development build does not save or print the key."},{"&OK"});return;}
  try {auto auth=run({{"action","auth"},{"token",std::string(key)}});Panel panel(ctx,auth.value("info",std::string()));panel.openWindowInForeground();}catch(const std::exception& e){error(e);}
#else
  ui::Alert::show("PixelLab native<<This development feature was not enabled in this build.||&OK");
#endif
 }
};
Command* CommandFactory::createPixelLabNativeCommand(){return new PixelLabNativeCommand;}
class PixelLabNativeSaveCommand final : public Command {
public: PixelLabNativeSaveCommand():Command("PixelLabNativeSave"){}
protected:
 bool onEnabled(Context* ctx) override{return ctx->isUIAvailable()&&ctx->activeDocument();}
 void onExecute(Context* ctx) override {
#ifdef ENABLE_PIXELLAB_NATIVE
  try{saveSprite(ctx);}catch(const std::exception& e){error(e);}
#else
  ui::Alert::show("PixelLab native<<This development feature was not enabled in this build.||&OK");
#endif
 }
};
Command* CommandFactory::createPixelLabNativeSaveCommand(){return new PixelLabNativeSaveCommand;}

}
