// TidyOS HTML5 edition: web screen server
//
// The desktop is still drawn into FabGL's normal 640x480 frame buffer (so a VGA monitor
// works too if one is plugged in). This task watches that buffer, sends the rows that
// changed to the browser over a WebSocket, and feeds the browser's keyboard and mouse
// back into TidyOS. One browser at a time; opening it somewhere else takes over.
//
// With no Wi-Fi set up (or if it can't connect) TidyOS starts a "TidyOS-Setup" hotspot,
// so a phone or PC can join it, open http://10.1.1.250 and use Settings to join home Wi-Fi.

#include "config.h"
#ifdef TIDYOS_HTML5

#include <WiFi.h>
#include "esp_wifi.h"
#include <DNSServer.h>
#include "freertos/stream_buffer.h"
#include "lwip/sockets.h"
#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"
#include "fabgl.h"
#include "core.h"
#include "screen.h"
#include "fileserver.h"
#include "net.h"

void inputWebKey(uint8_t usage, uint8_t mods, bool down);
void inputWebMouse(int x, int y, int wheel, uint8_t buttons);
void deskWeatherFromPage(const char * t);
void storeFromPage(const char * t);
void deskNewsFromPage(const char * t);
void deskTempsFromPage(const char * t);
void deskCityTempsFromPage(const char * t);
void deskFieldFromPage(const char * t);
void deskDropStart(int x, int y);
void deskDropped(const char * name);
void inputWebReleaseAll();
RGB888 themePal(int i);

static WiFiServer    s_srv(80);
static bool          s_listening = false;
static bool          s_netMode = false;   // the hotspot passes the devices' "is there internet?" checks (see apNetConfig)
static DNSServer     s_dns;
static volatile bool s_ap = false;
static WiFiClient    s_ws;
static volatile bool s_view = false;
static uint8_t *     s_prev = nullptr;   // what the browser has, row by row (PSRAM): only the changed part of a row is sent
static bool          s_rowOk[SCREEN_H];  // false = the browser has nothing for this row yet
static uint32_t      s_apSince = 0, s_connSince = 0;
static char          s_sentHome[80];      // last "home Wi-Fi" address sent to this browser
static char          s_mediaQ[4][300];    // media commands waiting for the page (small ring)
static volatile uint8_t s_mqW = 0, s_mqR = 0;
static portMUX_TYPE  s_mqMux = portMUX_INITIALIZER_UNLOCKED;
static volatile int  s_mState = -1, s_mPos = 0, s_mDur = 0;
static volatile int  s_mEpoch = 0;     // +1 for every new browser page: music it was playing is gone with the old one

// ---------------------------------------------------------------- the page
static const char PAGE[] = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover"><title>TidyOS Pro</title><style>
html,body{margin:0;height:100%;background:#1c1c1c;color:#ddd;font:14px system-ui,sans-serif;overflow:hidden;-webkit-text-size-adjust:100%;touch-action:none}
#bar{height:36px;display:flex;align-items:center;gap:6px;padding:0 8px;background:#0055aa;color:#fff;overflow-x:auto;overflow-y:hidden;white-space:nowrap;scrollbar-width:none;touch-action:pan-x;box-sizing:border-box;width:100%}
#bar::-webkit-scrollbar{display:none}#bar>*{flex:none}
#bar b{font-size:16px;margin-right:6px}#st{margin-left:auto;font-size:12px;opacity:.9;white-space:nowrap}
button{background:#fff;color:#0055aa;border:0;border-radius:5px;padding:5px 10px;font:inherit;cursor:pointer}
#wrap{position:absolute;top:36px;left:0;right:0;bottom:0;display:flex;align-items:center;justify-content:center}
canvas{background:#00aaaa;touch-action:none;outline:none;cursor:default}
#kb{position:absolute;left:0;top:0;width:1px;height:1px;opacity:0;border:0;padding:0}
#home{display:none;position:absolute;top:36px;left:0;right:0;z-index:2;background:#ffd84d;color:#111;padding:8px 12px;font-size:15px;text-align:center}
#home a{color:#0055aa;font-weight:600}
#msg{position:absolute;top:36px;left:0;right:0;bottom:0;display:flex;flex-direction:column;gap:14px;align-items:center;justify-content:center;background:rgba(0,0,0,.65);font-size:18px;text-align:center;white-space:pre-line;padding:0 16px}
@media(max-width:560px){#bt,#bf,#st{display:none}#bar{gap:4px;padding:0 4px}#bar b{font-size:14px;margin-right:2px}button{padding:4px 7px;font-size:13px}}
#zr{display:none;position:absolute;right:8px;bottom:8px;z-index:3;background:#0055aa;color:#fff;font-weight:600;opacity:.9}
</style></head><body>
<div id="bar"><b>TidyOS <span style="background:#ffd84d;color:#0055aa;border-radius:4px;padding:1px 6px;font-size:12px;vertical-align:2px">PRO</span></b><button id="bk">Keyboard</button><button id="bt">Task Manager</button><button id="bfi" title="The SD card and the board's storage: download and upload files" onclick="window.open('/files','_blank')">&#128193; Files</button><button id="br">Restart</button><button id="bs" style="background:#202a44;color:#fff" title="Deep sleep: almost no power. PRG wakes it.">&#9790; Deep Sleep</button><button id="bf">Full screen</button><button id="bz">Size: fit</button><span id="st">Connecting...</span></div>
<div id="home"></div><div id="iv" onclick="this.style.display='none'" style="display:none;position:fixed;inset:0;z-index:9;background:#000d;align-items:center;justify-content:center;cursor:zoom-out"><img style="max-width:96vw;max-height:94vh"></div><div id="wrap"><canvas id="c" width="640" height="480" tabindex="0"></canvas><button id="zr" title="Back to the whole screen">Fit</button></div>
<input id="kb" autocapitalize="off" autocomplete="off" autocorrect="off" spellcheck="false">
<div id="msg"><div id="mt">Connecting to TidyOS Pro...</div><button id="mb" style="display:none">Use it here</button></div>
<script>var MP=8081;
var c=document.getElementById('c'),g=c.getContext('2d'),o=document.createElement('canvas');o.width=640;o.height=480;window.tidyScreen=o;
var og=o.getContext('2d'),img=og.createImageData(640,480),px=new Uint32Array(img.data.buffer),ZK=1;
var openAt=0,flap=0,lostAt=0,home=null,ws=null,bpp=2,pal=new Uint32Array(8),dirty=false,rx=0,kicked=false,down={},mx=-1,my=-1,mb=0,mDirty=false;
var msg=document.getElementById('msg'),mt=document.getElementById('mt'),btn=document.getElementById('mb'),st=document.getElementById('st'),kb=document.getElementById('kb');
function show(t,b){mt.textContent=t;btn.style.display=b?'':'none';msg.style.display=t?'flex':'none';}
// sizing: whole device pixels when that fills most of the window (every pixel the same size = crisp text),
// smooth scaling otherwise; the Size button switches to exact 1:1 pixels (scroll if needed)
var mode=0;try{mode=+localStorage.tidySize||0;}catch(x){}
var Z=1,TX=0,TY=0;     // pinch zoom of the desktop: scale and shift (CSS px), set by two fingers
// "sharp bilinear": blow the screen up by a whole number with hard pixels, then let the browser
// shrink that a little smoothly. Every letter keeps the same shape at any window size.
function fit(){var r=window.devicePixelRatio||1,w=innerWidth,h=innerHeight-36,s=Math.min(w/640,h/480)*r,wrap=document.getElementById('wrap');
 if(mode==1)s=Math.max(1,Math.round(r));
 if(mode==2)s=Math.max(1,Math.floor(s+0.001));          // sharp: whole screen pixels, no smoothing
 ZK=Math.max(1,Math.min(Math.max(4,Math.ceil(s-0.001)),Math.ceil(s*Z-0.001)));if(c.width!=640*ZK){c.width=640*ZK;c.height=480*ZK;}   // zoomed in: drawn bigger, so it stays sharp
 c.style.width=(640*s/r)+'px';c.style.height=(480*s/r)+'px';c.style.imageRendering=mode==2?'pixelated':'auto';dirty=true;
 var big=640*s/r>w||480*s/r>h;wrap.style.overflow=mode==1?'auto':'hidden';wrap.style.alignItems=wrap.style.justifyContent=(mode==1&&big)?'flex-start':'center';
 document.getElementById('bz').textContent=mode==1?'Size: 1:1':mode==2?'Size: sharp':'Size: fit';}
addEventListener('resize',function(){Z=1;TX=TY=0;if(typeof zApply=='function')zApply();fit();});
function send(a){if(ws&&ws.readyState==1)ws.send(new Uint8Array(a));}
// the board has no internet of its own (its hotspot, at work or in the car): it asks this page to fetch a file
// (an update, say) over this device's internet, and the page passes it on in pieces. [23 status total] [22 data]... [24 ok]
function relay(url){var w0=ws;fetch(url,{cache:'no-store'}).then(function(r){var t=+(r.headers.get('content-length')||0);
  send([23,r.status&255,r.status>>8,t&255,t>>8&255,t>>16&255,t>>>24]);if(!r.ok||!r.body){send([24,0]);return;}
  var rd=r.body.getReader();
  (function pump(){if(ws!==w0||ws.readyState!=1){try{rd.cancel();}catch(e){}return;}
   if(ws.bufferedAmount>65536){setTimeout(pump,15);return;}
   rd.read().then(function(x){if(x.done){send([24,1]);return;}var v=x.value;
    for(var i=0;i<v.length;i+=4000){var n=Math.min(4000,v.length-i),c=new Uint8Array(n+1);c[0]=22;c.set(v.subarray(i,i+n),1);ws.send(c);}pump();})
   .catch(function(){send([24,0]);});})();})
 .catch(function(e){send([21].concat(Array.from(new TextEncoder().encode('relay: could not fetch '+url+' ('+e+')'))));send([23,0,0,0,0,0,0]);send([24,0]);});}
function connect(){
 kicked=false;if(!lostAt)show('Connecting to TidyOS Pro...');
 ws=new WebSocket('ws://'+location.host+'/ws');ws.binaryType='arraybuffer';
 var w0=ws;setTimeout(function(){if(w0.readyState==0)w0.close();},2000);   // a board that's restarting can leave this hanging: give up and try again (every second or so)
 ws.onopen=function(){lostAt=0;openAt=Date.now();show('');st.textContent='Connected';c.focus();var t=Math.floor(Date.now()/1000);send([6,t&255,t>>8&255,t>>16&255,t>>>24]);wxAt=0;fcAt=0;ctAt=0;nwAt=0;setTimeout(function(){weather();fieldWx();cityTemps();news();},300);};   // all at once, as soon as the screen is up
 ws.onmessage=function(e){var d=new Uint8Array(e.data);rx+=d.length;
  if(d[0]==1){bpp=d[1];for(var i=0;i<d[2];i++)pal[i]=0xff000000|d[5+i*3]<<16|d[4+i*3]<<8|d[3+i*3];}
  else if(d[0]==2){var y=d[1]|d[2]<<8,n=d[3],o=4,rb=bpp==1?80:bpp==2?160:240;
   for(var r=0;r<n;r++,y++,o+=rb){var p=y*640;
    if(bpp==1){for(var x=0;x<80;x++){var b=d[o+x];for(var i=7;i>=0;i--)px[p++]=pal[b>>i&1];}}
    else if(bpp==2){for(var x=0;x<160;x++){var b=d[o+x];px[p++]=pal[b>>6];px[p++]=pal[b>>4&3];px[p++]=pal[b>>2&3];px[p++]=pal[b&3];}}
    else{for(var k=0;k<80;k++){var q=o+k*3,v=d[q]|d[q+1]<<8|d[q+2]<<16;for(var i=0;i<8;i++)px[p++]=pal[v>>(21-3*i)&7];}}}
   dirty=true;}
  else if(d[0]==3){var cnt=d[1]|d[2]<<8,o=3;
   for(var sg=0;sg<cnt;sg++){var y=d[o]|d[o+1]<<8,a=d[o+2],len=d[o+3];o+=4;var oe=o+len;
    if(bpp==1){var p=y*640+a*8;for(;o<oe;o++){var b=d[o];for(var i=7;i>=0;i--)px[p++]=pal[b>>i&1];}}
    else if(bpp==2){var p=y*640+a*4;for(;o<oe;o++){var b=d[o];px[p++]=pal[b>>6];px[p++]=pal[b>>4&3];px[p++]=pal[b>>2&3];px[p++]=pal[b&3];}}
    else{var p=y*640+a/3*8;for(;o<oe;o+=3){var v=d[o]|d[o+1]<<8|d[o+2]<<16;for(var i=0;i<8;i++)px[p++]=pal[v>>(21-3*i)&7];}}}
   dirty=true;}
  else if(d[0]==8){var t=new TextDecoder().decode(d.subarray(1)).split('\n');ver=t[0];upBase=parseInt(t[1])||0;upAt=Date.now();}
  else if(d[0]==9)media(new TextDecoder().decode(d.subarray(1)));
  else if(d[0]==3){kicked=true;show('TidyOS was opened in another browser.',true);}
  else if(d[0]==7){var t=new TextDecoder().decode(d.subarray(1)).split('\n');home={ssid:t[0],ip:t[1],host:t[2]||''};
   var hb=document.getElementById('home');
   if(location.hostname!=home.ip&&location.hostname.slice(-6)!='.local'){var hn=(home.host||'')+'.local';hb.innerHTML='TidyOS is on <b></b> now. Switch this device to that Wi-Fi, then open <a href="http://'+home.ip+'/">http://'+home.ip+'</a>'+(home.host?' or <a href="http://'+hn+'/">http://'+hn+'</a>':'');hb.querySelector('b').textContent=home.ssid;hb.style.display='block';}
   else hb.style.display='none';}};
 ws.onclose=function(){st.textContent='Disconnected';if(!kicked){
  if(!lostAt)lostAt=Date.now();var secs=Math.round((Date.now()-lostAt)/1000);
  var t='TidyOS Pro is restarting (boot menu, then TidyOS Pro, or an update) - reconnecting every second... '+secs+' s';
  if(secs>90)t='Still no answer after '+secs+' s - trying every second. Check the board is on and on this Wi-Fi.';
  if(location.hostname=='10.1.1.250')t='Lost the TidyOS hotspot - trying again...\nIf TidyOS just joined your Wi-Fi, switch this device to '+(home?home.ssid:'that Wi-Fi')+' and open http://'+(home?home.ip:'the address on its screen');
  else if(home&&home.host)t+='\nIf you turned on the TidyOS hotspot: join Wi-Fi '+('TidyOS-'+home.host.slice(7).toUpperCase())+' (password tidyos123) and open http://10.1.1.250';
  show(t);var q=openAt&&Date.now()-openAt<4000;openAt=0;flap=q?flap+1:0;setTimeout(connect,flap>2?6000:1000);}};   // dropped again right after connecting (another page open on it?): slow down
}
btn.onclick=function(){connect();};
function frame(){if(dirty){og.putImageData(img,0,0);g.imageSmoothingEnabled=false;g.drawImage(o,0,0,640*ZK,480*ZK);dirty=false;}
 if(mDirty){mDirty=false;send([2,mx&255,mx>>8,my&255,my>>8,mb,0]);}requestAnimationFrame(frame);}
requestAnimationFrame(frame);
var ver='',upBase=-1,upAt=0,au=null,auBlocked=false;
var bT=null,bA=null,ac=null;
function beepWav(){var r=8000,n=r*0.7|0,b=new ArrayBuffer(44+n*2),v=new DataView(b),w=function(o,s){for(var i=0;i<s.length;i++)v.setUint8(o+i,s.charCodeAt(i));};
 w(0,'RIFF');v.setUint32(4,36+n*2,true);w(8,'WAVEfmt ');v.setUint32(16,16,true);v.setUint16(20,1,true);v.setUint16(22,1,true);v.setUint32(24,r,true);v.setUint32(28,r*2,true);v.setUint16(32,2,true);v.setUint16(34,16,true);w(36,'data');v.setUint32(40,n*2,true);
 for(var i=0;i<n;i++){var t=i/r,k=t%0.22,on=t<0.66&&k<0.13;v.setInt16(44+i*2,on?Math.sin(2*Math.PI*880*t)*12000*Math.min(1,(0.13-k)*60,k*400):0,true);}
 return URL.createObjectURL(new Blob([b],{type:'audio/wav'}));}
function beep(){try{if(!bA){bA=new Audio(beepWav());}bA.currentTime=0;bA.play().catch(function(){auBlocked=true;beepWA();});}catch(e){beepWA();}}
function beepWA(){try{if(!ac)ac=new(window.AudioContext||window.webkitAudioContext)();if(ac.state=='suspended')ac.resume();var t=ac.currentTime;
 for(var i=0;i<3;i++){var o=ac.createOscillator(),g=ac.createGain();o.frequency.value=880;o.connect(g);g.connect(ac.destination);g.gain.value=0;g.gain.setValueAtTime(0.3,t+i*0.22);g.gain.setValueAtTime(0,t+i*0.22+0.13);o.start(t+i*0.22);o.stop(t+i*0.22+0.15);}}catch(e){}}
function alarmBanner(on){var e=document.getElementById('alm');if(!e){e=document.createElement('div');e.id='alm';e.style.cssText='position:fixed;top:40px;left:50%;transform:translateX(-50%);z-index:20;background:#c00;color:#fff;font:bold 22px sans-serif;padding:12px 22px;border-radius:8px;cursor:pointer;display:none';e.textContent='⏰ Alarm! Click to silence';e.onclick=function(){clearInterval(bT);bT=null;alarmBanner(false);};document.body.appendChild(e);}
 e.style.display=on?'block':'none';document.title=on?'⏰ Alarm - TidyOS':'TidyOS';}
function unlockSound(){if(!bA){try{bA=new Audio(beepWav());bA.volume=0;bA.play().then(function(){bA.pause();bA.volume=1;}).catch(function(){bA.volume=1;});}catch(e){}}}
function mUrl(p){return location.protocol+'//'+location.hostname+':'+MP+p;}
function blog(t){try{send([21].concat(Array.from(new TextEncoder().encode(String(t).slice(0,200)))));}catch(e){}}
function auReport(){if(!au)return;var t=au.currentTime|0,u=isFinite(au.duration)?au.duration|0:0;send([8,au.ended?2:au.paused?0:1,t&255,t>>8&255,u&255,u>>8&255]);
 if('mediaSession' in navigator)try{navigator.mediaSession.playbackState=au.paused?'paused':'playing';}catch(e){}}
// the phone's lock screen / notification player and a computer's media keys: the song's name, and
// play, pause, stop and next work there too (next goes to TidyOS, which picks the song)
function auMeta(v){if(!('mediaSession' in navigator))return;try{
 var n=decodeURIComponent((v.split('path=')[1]||v).split('&')[0]).split('/').pop().replace(/\.[^.]+$/,''),a='TidyOS Pro',t=n,i=n.indexOf(' - ');
 if(i>0){a=n.slice(0,i);t=n.slice(i+3);}
 navigator.mediaSession.metadata=new MediaMetadata({title:t,artist:a,album:'TidyOS Pro'});
 if(auMeta.set)return;auMeta.set=1;var ms=navigator.mediaSession,h=function(k,f){try{ms.setActionHandler(k,f);}catch(e){}};
 h('play',function(){if(au)au.play().then(auReport).catch(function(){});});
 h('pause',function(){if(au){au.pause();auReport();}});
 h('stop',function(){if(au){au.pause();auReport();}});
 h('nexttrack',function(){send([25]);});
 h('previoustrack',function(){if(au){au.currentTime=0;auReport();}});}catch(e){}}
var midiCur=null;
function midiParse(d){var p=0,u16=function(){var v=d[p]<<8|d[p+1];p+=2;return v;},u32=function(){var v=(d[p]<<24|d[p+1]<<16|d[p+2]<<8|d[p+3])>>>0;p+=4;return v;},
 vlq=function(){var v=0,b;do{b=d[p++];v=v*128+(b&127);}while(b&128&&p<d.length);return v;};
 for(var o=0;o<d.length-4&&o<512;o++)if(d[o]==77&&d[o+1]==84&&d[o+2]==104&&d[o+3]==100)break;if(o>0&&o<d.length-4)d=d.subarray(o);if(String.fromCharCode(d[0],d[1],d[2],d[3])!='MThd')throw 'not midi';p=4;var hl=u32();u16();var nt=u16(),div=u16();if(div&0x8000)div=480;p=8+hl;
 var ev=[];for(var k=0;k<nt&&p<d.length;k++){var id=String.fromCharCode(d[p],d[p+1],d[p+2],d[p+3]);p+=4;var len=u32(),end=p+len,tick=0,rs=0;if(id!='MTrk'){p=end;continue;}
  while(p<end){tick+=vlq();var st=d[p];if(st&128)p++;else st=rs;
   if(st==0xFF){var ty=d[p++],l=vlq();if(ty==0x51&&l==3)ev.push({k:tick,y:2,tp:d[p]<<16|d[p+1]<<8|d[p+2]});p+=l;}
   else if(st==0xF0||st==0xF7){p+=vlq();}
   else{rs=st;var c=st&15,h=st>>4,a=d[p++],b=(h==12||h==13)?0:d[p++];
    if(h==9&&b>0)ev.push({k:tick,y:1,c:c,n:a,v:b});else if(h==8||(h==9&&b==0))ev.push({k:tick,y:0,c:c,n:a});else if(h==12)ev.push({k:tick,y:3,c:c,n:a});}}
  p=end;}
 var ord=[2,3,0,1];ev.sort(function(x,y){return x.k-y.k||ord[x.y]-ord[y.y];});var tp=500000,lt=0,ts=0;
 for(var i=0;i<ev.length;i++){ts+=(ev[i].k-lt)*tp/div/1e6;lt=ev[i].k;ev[i].t=ts;if(ev[i].y==2)tp=ev[i].tp;}
 return ev;}
var noiseBuf=null;
function midiNote(m,e,at){var c=m.ctx,g=c.createGain();g.connect(m.out);
 if(e.c==9){if(!noiseBuf){noiseBuf=c.createBuffer(1,c.sampleRate/2,c.sampleRate);var q=noiseBuf.getChannelData(0);for(var i=0;i<q.length;i++)q[i]=Math.random()*2-1;}
  var n=e.n;if(n==35||n==36){var o=c.createOscillator();o.frequency.setValueAtTime(150,at);o.frequency.exponentialRampToValueAtTime(40,at+0.15);o.connect(g);g.gain.setValueAtTime(0.9*e.v/127,at);g.gain.exponentialRampToValueAtTime(0.001,at+0.2);o.start(at);o.stop(at+0.22);return;}
  var s=c.createBufferSource(),f=c.createBiquadFilter();s.buffer=noiseBuf;s.connect(f);f.connect(g);var hat=n==42||n==44||n==46||n>=49;f.type=hat?'highpass':'bandpass';f.frequency.value=hat?7000:1800;
  var len=n==46||n>=49?0.3:hat?0.05:0.15;g.gain.setValueAtTime(0.5*e.v/127,at);g.gain.exponentialRampToValueAtTime(0.001,at+len);s.start(at);s.stop(at+len+0.02);return;}
 var key=e.c*128+e.n;if(m.on[key])midiOff(m,key,at);if(m.count>24)return;
 var o=c.createOscillator();o.type=e.n<48?'sawtooth':(m.prog[e.c]>=40&&m.prog[e.c]<80?'square':'triangle');o.frequency.value=440*Math.pow(2,(e.n-69)/12);o.connect(g);
 var vol=(o.type=='triangle'?0.22:0.07)*e.v/127;g.gain.setValueAtTime(0,at);g.gain.linearRampToValueAtTime(vol,at+0.01);g.gain.setTargetAtTime(vol*0.6,at+0.05,0.3);
 o.start(at);m.on[key]={o:o,g:g};m.count++;}
function midiOff(m,key,at){var x=m.on[key];if(!x)return;x.g.gain.cancelScheduledValues(at);x.g.gain.setTargetAtTime(0,at,0.05);x.o.stop(at+0.3);delete m.on[key];m.count--;}
function midiPump(){var m=midiCur;if(!m||m.paused)return;var now=m.ctx.currentTime;
 while(m.i<m.ev.length&&m.ev[m.i].t+m.t0<now+0.7){var e=m.ev[m.i++],at=Math.max(now,e.t+m.t0);
  if(e.y==1)midiNote(m,e,at);else if(e.y==0)midiOff(m,e.c*128+e.n,at);else if(e.y==3)m.prog[e.c]=e.n;}
 if(m.i>=m.ev.length&&now>m.t0+m.dur+0.5){m.ended=true;midiReport();clearInterval(m.timer);}}
function midiReport(){var m=midiCur;if(!m)return;var t=m.ended?m.dur:Math.max(0,m.ctx.currentTime-m.t0);t=t|0;var u=m.dur|0;send([8,m.ended?2:m.paused?0:1,t&255,t>>8&255,u&255,u>>8&255]);}
function midiStop(){if(!midiCur)return;var m=midiCur;midiCur=null;clearInterval(m.timer);clearInterval(m.rep);try{m.ctx.close();}catch(e){}}
function midiPlay(url,vol){midiStop();if(au){au.pause();}
 fetch(url).then(function(r){return r.arrayBuffer();}).then(function(b){var ev=midiParse(new Uint8Array(b)),C=window.AudioContext||window.webkitAudioContext,ctx=new C();
  var m={ctx:ctx,ev:ev,i:0,on:{},count:0,prog:[],paused:false,ended:false,out:ctx.createGain()};m.out.gain.value=vol;m.out.connect(ctx.destination);
  m.dur=ev.length?ev[ev.length-1].t:0;m.t0=ctx.currentTime+0.15;midiCur=m;if(ctx.state=='suspended'){ctx.resume();auBlocked=true;}
  m.timer=setInterval(midiPump,150);m.rep=setInterval(midiReport,1000);midiPump();}).catch(function(){send([8,2,0,0,0,0]);});}

function openBanner(u,n){var w=null;try{w=window.open(u,'_blank');}catch(e){}if(w)return;
 var e=document.getElementById('ob');if(!e){e=document.createElement('div');e.id='ob';e.style.cssText='position:fixed;top:44px;left:50%;transform:translateX(-50%);z-index:21;background:#0055aa;color:#fff;font:bold 18px sans-serif;padding:10px 14px;border-radius:8px;display:none;gap:10px;align-items:center;box-shadow:0 4px 16px #0008';document.body.appendChild(e);}
 e.innerHTML='';var b=document.createElement('button');b.textContent='▶ Play '+n;b.style.cssText='font:bold 18px sans-serif;padding:8px 14px;cursor:pointer';
 b.onclick=function(){window.open(u,'_blank');e.style.display='none';};var x=document.createElement('button');x.textContent='✕';x.style.cssText='font:16px sans-serif;padding:6px 10px;cursor:pointer';x.onclick=function(){e.style.display='none';};
 e.appendChild(b);e.appendChild(x);e.style.display='flex';}
var wxAt=0;
function wxSend(s){send([12].concat(Array.from(new TextEncoder().encode(s.slice(0,62)))));}
var geoP=null,geoAt=0;function geo(){if(geoP&&Date.now()-geoAt<6*3600000)return geoP;geoAt=Date.now();   // one lookup for weather and the field panel (kept 6 h)
 geoP=geo1().catch(function(e){geoP=null;throw e;});return geoP;}
function geo1(){var tries=[['https://ipwho.is/',function(j){return j.success!==false&&j.latitude!=null?{lat:j.latitude,lon:j.longitude,city:j.city}:null;}],
  ['https://ipapi.co/json/',function(j){return j.latitude!=null?{lat:j.latitude,lon:j.longitude,city:j.city}:null;}],
  ['http://ip-api.com/json/?fields=status,city,lat,lon',function(j){return j.status=='success'?{lat:j.lat,lon:j.lon,city:j.city}:null;}]],i=0;
 return new Promise(function(ok,bad){(function next(){if(i>=tries.length){bad('location lookup blocked');return;}var t=tries[i++];
  fetch(t[0],{signal:AbortSignal.timeout?AbortSignal.timeout(5000):undefined}).then(function(r){return r.json();}).then(function(j){var g=t[1](j);if(g)ok(g);else next();}).catch(next);})();});}
function weather(){if(!ws||ws.readyState!=1||Date.now()-wxAt<10*60000)return;wxAt=Date.now();
 geo().then(function(g){
  return fetch('https://api.open-meteo.com/v1/forecast?latitude='+g.lat+'&longitude='+g.lon+'&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min&hourly=temperature_2m&past_days=4&temperature_unit=fahrenheit&timezone=auto&forecast_days=1')
   .then(function(r){return r.json();}).then(function(w){
    try{var h=w.hourly,now=Date.now(),pts=[];for(var i=0;i<h.time.length;i++){if(new Date(h.time[i]).getTime()>now)break;pts.push(h.temperature_2m[i]);}
     pts=pts.slice(-97);var s=[];for(var i=pts.length-1;i>=0;i-=3)s.unshift(pts[i]==null?0:Math.round(pts[i]));   // every 3 h, newest (this hour) last
     send([17].concat(Array.from(new TextEncoder().encode(s.join(',')))));}catch(e){}
    wxSend(Math.round(w.current.temperature_2m)+'|'+w.current.weather_code+'|'+Math.round(w.daily.temperature_2m_max[0])+'|'+Math.round(w.daily.temperature_2m_min[0])+'|'+(+g.lat).toFixed(2)+'|'+(+g.lon).toFixed(2)+'|'+(g.city||'').replace(/[^\x20-\x7e]/g,'').slice(0,24));})
   .catch(function(){wxSend('E|weather service unreachable|'+(+g.lat).toFixed(2)+'|'+(+g.lon).toFixed(2));throw 1;});})
 .catch(function(e){if(e!==1)wxSend('E|'+(typeof e=='string'?e:'no internet from this browser'));wxAt=Date.now()-9.5*60000;});}
setInterval(weather,15000);

// ---- TidyOS Pro: field conditions for the desktop panel (wind, humidity, storms, tropo ducting,
// space weather, NWS alerts). Every series is the last 4 days, one point per 3 hours, newest last.
var fcAt=0,fcGeo=null;
function fcSend(k,v){send([19].concat(Array.from(new TextEncoder().encode((k+'|'+v).slice(0,1000)))));}
function every3(t,v,now){var pts=[];for(var i=0;i<t.length;i++){if(new Date(t[i]).getTime()>now)break;pts.push(v[i]);}pts=pts.slice(-97);var s=[];for(var i=pts.length-1;i>=0;i-=3)s.unshift(pts[i]);return s;}
function max3(t,v,now){var pts=[];for(var i=0;i<t.length;i++){if(new Date(t[i]).getTime()>now)break;pts.push(v[i]);}pts=pts.slice(-97);var s=[];for(var i=pts.length-1;i>=0;i-=3){var m=null;for(var k=i;k>i-3&&k>=0;k--)if(pts[k]!=null&&(m==null||pts[k]>m))m=pts[k];s.unshift(m);}return s;}
function refr(tC,rh,pHpa){if(tC==null||rh==null||pHpa==null)return null;var T=tC+273.15,es=6.112*Math.exp(17.67*tC/(tC+243.5)),e=rh/100*es;return 77.6/T*(pHpa+4810*e/T);}
function fieldWx(){if(!ws||ws.readyState!=1||Date.now()-fcAt<10*60000)return;fcAt=Date.now();
 geo().then(function(g){fcGeo=g;var now=Date.now();
  fetch('https://api.open-meteo.com/v1/forecast?latitude='+g.lat+'&longitude='+g.lon+'&hourly=wind_speed_10m,wind_gusts_10m,wind_direction_10m,relative_humidity_2m,dew_point_2m,cape,weather_code,temperature_2m,surface_pressure,temperature_925hPa,relative_humidity_925hPa,geopotential_height_925hPa&current=wind_speed_10m,wind_gusts_10m,wind_direction_10m,relative_humidity_2m,dew_point_2m,pressure_msl&wind_speed_unit=mph&past_days=4&forecast_days=1&timezone=auto')
  .then(function(r){return r.json();}).then(function(w){var h=w.hourly,T=h.time,r0=function(a){return a.map(function(x){return x==null?'':Math.round(x);}).join(',');};
   fcSend('wind',r0(every3(T,h.wind_speed_10m,now)));fcSend('gust',r0(max3(T,h.wind_gusts_10m,now)));
   fcSend('hum',r0(every3(T,h.relative_humidity_2m,now)));fcSend('cape',r0(max3(T,h.cape,now)));
   var wc=max3(T,h.weather_code,now);fcSend('thun',wc.map(function(c){return c!=null&&c>=95?1:0;}).join(','));
   // refractivity gradient, surface to the 925 hPa level, in N units per km (standard about -40;
   // -79 or lower bends signals further than normal, -157 or lower is a duct: 900 MHz carries far)
   var d=[];for(var i=0;i<T.length;i++){var ns=refr(h.temperature_2m[i],h.relative_humidity_2m[i],h.surface_pressure[i]),n9=refr(h.temperature_925hPa[i],h.relative_humidity_925hPa[i],925),z=h.geopotential_height_925hPa[i]-(w.elevation||0);
     d.push(ns==null||n9==null||!(z>50)?null:(n9-ns)/z*1000);}
   fcSend('duct',r0(every3(T,d,now)));
   var c=w.current;fcSend('now',[Math.round(c.wind_speed_10m),Math.round(c.wind_gusts_10m),Math.round(c.wind_direction_10m),Math.round(c.relative_humidity_2m),Math.round(c.dew_point_2m*9/5+32),(c.pressure_msl*0.02953).toFixed(2)].join(','));})
  .catch(function(){fcAt=Date.now()-9.5*60000;});
  // US weather alerts (thunderstorms, wind, winter storms) from the National Weather Service
  fetch('https://api.weather.gov/alerts/active?point='+(+g.lat).toFixed(4)+','+(+g.lon).toFixed(4))
   .then(function(r){return r.json();}).then(function(j){var a=(j.features||[]).map(function(f){return plain(f.properties.event||'')+(f.properties.expires?' until '+new Date(f.properties.expires).toLocaleTimeString([], {hour:'numeric',minute:'2-digit'}):'');});
    fcSend('alert',a.length?a.slice(0,3).join('\n'):'-');}).catch(function(){fcSend('alert','?');});
 }).catch(function(){fcAt=Date.now()-9.5*60000;});
 // space weather: NOAA SWPC (solar flux, Kp, X-ray)
 fetch('https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json').then(function(r){return r.json();}).then(function(a){
   var rows=a.filter(function(x){return Array.isArray(x)?!isNaN(parseFloat(x[1])):x&&x.Kp!=null;}).map(function(x){return Array.isArray(x)?parseFloat(x[1]):+x.Kp;});
   fcSend('kp',rows.slice(-32).map(function(k){return Math.round(k*10);}).join(','));}).catch(function(){});
 fetch('https://services.swpc.noaa.gov/json/f107_cm_flux.json').then(function(r){return r.json();}).then(function(a){var x=a[a.length-1];var ok=a.filter(function(e){return e.flux!=null;});if(ok.length)x=ok.reduce(function(p,q){return new Date(q.time_tag)>new Date(p.time_tag)?q:p;});fcSend('sfi',Math.round(x.flux));}).catch(function(){});
 fetch('https://services.swpc.noaa.gov/json/goes/primary/xrays-6-hour.json').then(function(r){return r.json();}).then(function(a){var l=a.filter(function(e){return e.energy=='0.1-0.8nm';});if(!l.length)return;var f=l[l.length-1].flux,c='A',m=1e-8;
   if(f>=1e-4){c='X';m=1e-4;}else if(f>=1e-5){c='M';m=1e-5;}else if(f>=1e-6){c='C';m=1e-6;}else if(f>=1e-7){c='B';m=1e-7;}fcSend('xray',c+(f/m).toFixed(1));}).catch(function(){});}
setInterval(fieldWx,15000);
var ctAt=0;function cityTemps(){if(!ws||ws.readyState!=1||Date.now()-ctAt<15*60000)return;ctAt=Date.now();   // Moscow, Los Angeles, Tokyo
 fetch('https://api.open-meteo.com/v1/forecast?latitude=55.76,34.05,35.68&longitude=37.62,-118.24,139.69&current=temperature_2m&temperature_unit=fahrenheit')
  .then(function(r){return r.json();}).then(function(a){if(!Array.isArray(a))a=[a];
   send([18].concat(Array.from(new TextEncoder().encode(a.map(function(c){return Math.round(c.current.temperature_2m);}).join(',')))));})
  .catch(function(){var L=[[55.76,37.62],[34.05,-118.24],[35.68,139.69]];        // one at a time instead
   Promise.all(L.map(function(c){return fetch('https://api.open-meteo.com/v1/forecast?latitude='+c[0]+'&longitude='+c[1]+'&current=temperature_2m&temperature_unit=fahrenheit').then(function(r){return r.json();}).then(function(j){return Math.round(j.current.temperature_2m);});}))
    .then(function(t){send([18].concat(Array.from(new TextEncoder().encode(t.join(',')))));}).catch(function(){ctAt=Date.now()-14.5*60000;});});}
setInterval(cityTemps,15000);
function rep13(s){send([13].concat(Array.from(new TextEncoder().encode(s.slice(0,1000)))));}
function store(v){var a=v.split('\t'),op=a[0],R=(a[1]||'').replace(/\/+$/,''),B=a[2];
 var chk=function(r){return r.json().then(function(j){if(!r.ok||j.error)throw j.error||('error '+r.status);return j;},function(){throw 'the relay gave an odd answer ('+r.status+')';});};
 var err=function(e){rep13(op+'\t'+(typeof e=='string'?e:'could not reach the relay'));};
 var F=function(n){return R+'/file?board='+B+'&name='+encodeURIComponent(n);};
 if(op=='L')fetch(R+'/list?board='+B).then(chk).then(function(j){var s='L\tok\t'+j.used+'\t'+j.max;j.files.forEach(function(f){s+='\n'+f.name.replace(/[^\x20-\x7e]/g,'_')+'\t'+f.size;});rep13(s);}).catch(err);
 else if(op=='U')fetch(mUrl(a[3])).then(function(r){if(!r.ok)throw 'could not read the file on the board';return r.blob();}).then(function(b){return fetch(F(a[4]),{method:'PUT',body:b});}).then(chk).then(function(){rep13('U\tok\t'+a[4]);}).catch(err);
 else if(op=='D')fetch(F(a[3])).then(function(r){if(!r.ok)throw 'not in the store';return r.blob();}).then(function(b){return fetch(mUrl(a[4]),{method:'PUT',body:b});}).then(function(r){if(!r.ok)throw 'could not save it on the board';rep13('D\tok\t'+a[3]);}).catch(err);
 else if(op=='X')fetch(F(a[3]),{method:'DELETE'}).then(chk).then(function(){rep13('X\tok\t'+a[3]);}).catch(err);}
var nwAt=0;function plain(t){return t.replace(/[‘’]/g,"'").replace(/[“”]/g,'"').replace(/[–—]/g,'-').replace(/…/g,'...').replace(/&amp;/g,'&').replace(/[^\x20-\x7e]/g,'').trim();}
function news(){if(!ws||ws.readyState!=1||Date.now()-nwAt<15*60000)return;nwAt=Date.now();
 var feeds=['https://feeds.npr.org/1001/rss.xml','https://feeds.bbci.co.uk/news/rss.xml'],i=0;
 (function next(){if(i>=feeds.length){nwAt=Date.now()-14.5*60000;return;}var f=feeds[i++];
  fetch('https://api.rss2json.com/v1/api.json?rss_url='+encodeURIComponent(f)).then(function(r){return r.json();}).then(function(j){
   if(j.status!='ok'||!j.items||!j.items.length)throw 0;var t=j.items.slice(0,6).map(function(x){return plain(x.title||'').slice(0,120);});
   send([14].concat(Array.from(new TextEncoder().encode(t.join('\n')))));}).catch(next);})();}
setInterval(news,15000);
var dropQ=[];
function note(t,ms){var e=document.getElementById('dn');if(!e){e=document.createElement('div');e.id='dn';e.style.cssText='position:fixed;bottom:18px;right:18px;z-index:22;background:#222;color:#fff;font:15px sans-serif;padding:10px 14px;border-radius:8px;box-shadow:0 4px 16px #0008';document.body.appendChild(e);}
 e.textContent=t;e.style.display='block';clearTimeout(e.t);if(ms)e.t=setTimeout(function(){e.style.display='none';},ms);}
addEventListener('dragover',function(e){e.preventDefault();e.dataTransfer.dropEffect='copy';},false);
addEventListener('drop',function(e){e.preventDefault();var fs=[].slice.call(e.dataTransfer.files||[]);if(!fs.length)return;dropRaw=e.altKey;
 if(!ws||ws.readyState!=1){note('TidyOS is not connected.',4000);return;}dropQ=dropQ.concat(fs);note('Getting TidyOS ready for '+fs.length+' file'+(fs.length>1?'s':'')+'...');
 var r=c.getBoundingClientRect(),x=(e.clientX-r.left)*640/r.width|0,y=(e.clientY-r.top)*480/r.height|0;if(x<0||y<0||x>639||y>479){x=65535;y=65535;}
 send([15,x&255,x>>8&255,y&255,y>>8&255]);},false);
function cloudNext(R,B){var f=dropQ.shift();if(!f)return;var nm=f.name.replace(/[^A-Za-z0-9 ._()-]/g,'_').slice(-80).replace(/^\./,'_');
 if(f.size>25*1048576){note(nm+' is over 25 MB - skipped.',6000);cloudNext(R,B);return;}
 note('Uploading '+nm+' to the cloud ('+Math.ceil(f.size/1024)+' KB)...');
 fetch(R.replace(/\/+$/,'')+'/file?board='+B+'&name='+encodeURIComponent(nm),{method:'PUT',body:f}).then(function(r){return r.json().then(function(j){if(!r.ok||j.error)throw j.error||('error '+r.status);});})
  .then(function(){rep13('U\tok\t'+nm);note('Uploaded '+nm+' to Git-Store.',dropQ.length?0:5000);})
  .catch(function(e){var m=typeof e=='string'?e:'could not reach the relay';rep13('U\t'+m);note('Couldn\'t upload '+nm+': '+m,8000);})
  .then(function(){cloudNext(R,B);});}
// Music shrinker (Tidy Pro Plus): MP3 / WAV / M4A / FLAC / OGG -> Opus 48 kbps in an Ogg file (.opus),
// done in the browser before the file goes to the board. WebCodecs encodes; this writes the Ogg pages.
var SHRINK_KBPS=48,OPUS_WORKER='https://cdn.jsdelivr.net/npm/opus-recorder@8.0.5/dist/encoderWorker.min.js',SHRINK_FORCE_WASM=false;
function oggCrc(){var t=oggCrc.t;if(!t){t=oggCrc.t=new Uint32Array(256);for(var i=0;i<256;i++){var r=i<<24;for(var k=0;k<8;k++)r=r&0x80000000?(r<<1)^0x04c11db7:r<<1;t[i]=r>>>0;}}return t;}
function oggPage(pkts,granule,serial,seq,flags){            // pkts: Uint8Array[]; returns one page
 var segs=[],body=0;pkts.forEach(function(p){var n=p.length;while(n>=255){segs.push(255);n-=255;}segs.push(n);body+=p.length;});
 var h=27+segs.length,b=new Uint8Array(h+body),v=new DataView(b.buffer);
 b.set([79,103,103,83,0,flags]);v.setUint32(6,granule%4294967296,true);v.setUint32(10,Math.floor(granule/4294967296),true);
 v.setUint32(14,serial,true);v.setUint32(18,seq,true);b[26]=segs.length;b.set(segs,27);
 var o=h;pkts.forEach(function(p){b.set(p,o);o+=p.length;});
 var t=oggCrc(),c=0;for(var i=0;i<b.length;i++)c=((c<<8)^t[((c>>>24)^b[i])&255])>>>0;v.setUint32(22,c,true);return b;}
function oggPagePart(part,granule,serial,seq,cont,more){
 var segs=[],n=part.length;while(n>=255){segs.push(255);n-=255;}if(!more)segs.push(n);else if(n)segs.push(n);
 var h=27+segs.length,b=new Uint8Array(h+part.length),v=new DataView(b.buffer);
 b.set([79,103,103,83,0,cont]);v.setUint32(6,more?4294967295:granule,true);v.setUint32(10,more?4294967295:0,true);
 v.setUint32(14,serial,true);v.setUint32(18,seq,true);b[26]=segs.length;b.set(segs,27);b.set(part,h);
 var t=oggCrc(),c=0;for(var i=0;i<b.length;i++)c=((c<<8)^t[((c>>>24)^b[i])&255])>>>0;v.setUint32(22,c,true);return b;}
function opusHead(ch,preskip){var b=new Uint8Array(19),v=new DataView(b.buffer);b.set([79,112,117,115,72,101,97,100,1,ch]);v.setUint16(10,preskip,true);v.setUint32(12,48000,true);return b;}
function opusTags(list){var te=new TextEncoder(),vend=te.encode('Tidy Pro Plus'),cs=(list||[]).map(function(x){return te.encode(x);}),n=8+4+vend.length+4;
 cs.forEach(function(c){n+=4+c.length;});var b=new Uint8Array(n),v=new DataView(b.buffer),o=0;
 b.set([79,112,117,115,84,97,103,115]);o=8;v.setUint32(o,vend.length,true);o+=4;b.set(vend,o);o+=vend.length;v.setUint32(o,cs.length,true);o+=4;
 cs.forEach(function(c){v.setUint32(o,c.length,true);o+=4;b.set(c,o);o+=c.length;});return b;}
// MP3 tags (ID3v2.3 / 2.4): title, artist, album, track, year, genre and the cover picture
function readId3(u8){
 var t={};if(u8.length<10||u8[0]!=73||u8[1]!=68||u8[2]!=51)return t;
 var ver=u8[3],ss=function(o){return (u8[o]&127)<<21|(u8[o+1]&127)<<14|(u8[o+2]&127)<<7|(u8[o+3]&127);},end=10+ss(6),o=10;
 if(u8[5]&64)o+=ver==4?ss(10):(u8[10]<<24|u8[11]<<16|u8[12]<<8|u8[13])+4;      // extended header
 function txt(a,b,enc){var d=u8.subarray(a,b);try{
  if(enc==0)return new TextDecoder('latin1').decode(d).replace(/\0.*$/,'');
  if(enc==3)return new TextDecoder('utf-8').decode(d).replace(/\0.*$/,'');
  var le=enc==1&&d[0]==255&&d[1]==254,be=enc==2||(enc==1&&d[0]==254&&d[1]==255);if(enc==1)d=d.subarray(2);
  return new TextDecoder(le||!be?'utf-16le':'utf-16be').decode(d).replace(/\0.*$/,'');}catch(e){return '';}}
 var map={TIT2:'TITLE',TPE1:'ARTIST',TALB:'ALBUM',TRCK:'TRACKNUMBER',TYER:'DATE',TDRC:'DATE',TCON:'GENRE',TPE2:'ALBUMARTIST'};
 while(o+10<=end&&o+10<=u8.length){
  var id=String.fromCharCode(u8[o],u8[o+1],u8[o+2],u8[o+3]);if(!/^[A-Z0-9]{4}$/.test(id))break;
  var sz=ver==4?ss(o+4):(u8[o+4]<<24|u8[o+5]<<16|u8[o+6]<<8|u8[o+7])>>>0,a=o+10,b=Math.min(a+sz,u8.length);
  if(map[id]&&b>a)t[map[id]]=txt(a+1,b,u8[a]).trim();
  else if(id=='APIC'&&b>a+4&&!t.cover){var enc=u8[a],p=a+1,q=p;while(q<b&&u8[q])q++;var mime=txt(p,q,0);p=q+2;      // mime, picture type
   if(enc==1||enc==2){while(p+1<b&&(u8[p]||u8[p+1]))p+=2;p+=2;}else{while(p<b&&u8[p])p++;p++;}                 // description
   if(p<b)t.cover={mime:mime.indexOf('/')>0?mime:'image/jpeg',data:u8.slice(p,b)};}
  o=a+sz;
 }
 return t;
}
// the cover, made small (up to 300 px, JPEG) so it costs ~20-40 KB, as a FLAC picture block in base64
function coverBlock(c){
 return createImageBitmap(new Blob([c.data],{type:c.mime})).then(function(img){
  var s=Math.min(1,300/Math.max(img.width,img.height)),w=Math.max(1,Math.round(img.width*s)),h=Math.max(1,Math.round(img.height*s));
  var cv=document.createElement('canvas');cv.width=w;cv.height=h;cv.getContext('2d').drawImage(img,0,0,w,h);
  function enc(q){return new Promise(function(r){cv.toBlob(r,'image/jpeg',q);}).then(function(bl){return bl.arrayBuffer();}).then(function(ab){
   if(ab.byteLength>45000&&q>0.4)return enc(q-0.15);return new Uint8Array(ab);});}
  return enc(0.82).then(function(jp){
   var mime=new TextEncoder().encode('image/jpeg'),blk=new Uint8Array(32+mime.length+jp.length),v=new DataView(blk.buffer),o=0;
   v.setUint32(o,3);o+=4;v.setUint32(o,mime.length);o+=4;blk.set(mime,o);o+=mime.length;v.setUint32(o,0);o+=4;
   v.setUint32(o,w);o+=4;v.setUint32(o,h);o+=4;v.setUint32(o,24);o+=4;v.setUint32(o,0);o+=4;v.setUint32(o,jp.length);o+=4;blk.set(jp,o);
   var s2='';for(var i=0;i<blk.length;i+=0x8000)s2+=String.fromCharCode.apply(null,blk.subarray(i,i+0x8000));return btoa(s2);});
 }).catch(function(){return null;});
}
function canShrink(f){return typeof OfflineAudioContext!='undefined'&&typeof Worker!='undefined'&&/\.(mp3|wav|m4a|aac|flac|ogg|oga|wma|aif|aiff)$/i.test(f.name);}
// Without WebCodecs (pages on plain http, like the board's own desktop, don't get it): libopus in a worker
// (opus-recorder's encoder, loaded from the CDN). Resolves to {pre: pre-skip, pages: its audio pages}.
function opusWorkerEncode(chans,len,ch,progress){
 return fetch(OPUS_WORKER).then(function(r){if(!r.ok)throw new Error('could not load the Opus encoder');return r.text();}).then(function(code){
  return new Promise(function(res,rej){
   var url=URL.createObjectURL(new Blob([code],{type:'text/javascript'})),w=new Worker(url),pages=[],pre=312,hdr=0;
   w.onerror=function(e){rej(new Error(e.message||'encoder failed'));};
   w.onmessage=function(e){var d=e.data;
    if(d&&d.message=='ready'){w.postMessage({command:'getHeaderPages'});feed();}
    else if(d&&d.message=='done'){w.terminate();URL.revokeObjectURL(url);res({pre:pre,pages:pages});}
    else if(d&&d.page){var pg=new Uint8Array(d.page);
     if(hdr<2&&pg[28]==79&&pg[29]==112&&pg[30]==117&&pg[31]==115){if(pg[32]==72)pre=pg[38]|pg[39]<<8;hdr++;return;}   // its own OpusHead / OpusTags: ours replace them
     pages.push(pg);}};
   w.postMessage({command:'init',encoderSampleRate:48000,originalSampleRate:48000,numberOfChannels:ch,encoderBitRate:SHRINK_KBPS*1000,encoderApplication:2049,encoderFrameSize:20,maxFramesPerPage:40,resampleQuality:3});
   var i=0,STEP=48000;
   function feed(){var end=Math.min(len,i+STEP*20);
    for(;i<end;i+=STEP){var n=Math.min(STEP,len-i),b=[];for(var k=0;k<ch;k++)b.push(chans[k].slice(i,i+n));w.postMessage({command:'encode',buffers:b});}
    if(progress)progress(Math.round(i*100/len));
    if(i<len)setTimeout(feed,0);else w.postMessage({command:'done'});}
  });
 });
}
// a page from the worker, renumbered after our own header pages (and its checksum redone)
function oggRenumber(pg,seq,eos){var b=pg.slice(),v=new DataView(b.buffer);v.setUint32(18,seq,true);if(eos)b[5]|=4;v.setUint32(22,0,true);
 var t=oggCrc(),c=0;for(var i=0;i<b.length;i++)c=((c<<8)^t[((c>>>24)^b[i])&255])>>>0;v.setUint32(22,c,true);return b;}
// resolves to a new .opus File, or the original when it can't be made smaller
function shrinkAudio(f,progress){
 var comments=[];
 return f.arrayBuffer().then(function(ab){
  var t=readId3(new Uint8Array(ab));
  Object.keys(t).forEach(function(k){if(k!='cover'&&t[k])comments.push(k+'='+t[k]);});
  var art=t.cover?coverBlock(t.cover).then(function(b64){if(b64)comments.push('METADATA_BLOCK_PICTURE='+b64);}):Promise.resolve();
  var ac=new OfflineAudioContext(2,48000,48000);                 // decoding here resamples to 48 kHz
  return art.then(function(){return ac.decodeAudioData(ab);});
 }).then(function(buf){
  var ch=Math.min(2,buf.numberOfChannels),len=buf.length,packets=[],err=null;
  if(typeof AudioEncoder=='undefined'||SHRINK_FORCE_WASM){
   var cs=[];for(var k=0;k<ch;k++)cs.push(buf.getChannelData(k));
   return opusWorkerEncode(cs,len,ch,progress).then(function(r){
    var serial=(Math.random()*4294967295)>>>0,seq=0,out=[];
    if(r.pages.length){var v0=new DataView(r.pages[0].buffer);serial=v0.getUint32(14,true);}
    out.push(oggPage([opusHead(ch,r.pre)],0,serial,seq++,2));
    var tg=opusTags(comments);
    for(var at=0;at<tg.length;at+=255*250){var part=tg.subarray(at,Math.min(tg.length,at+255*250));
     out.push(oggPagePart(part,0,serial,seq++,at?1:0,at+255*250<tg.length));}
    r.pages.forEach(function(pg,j){out.push(oggRenumber(pg,seq++,j==r.pages.length-1));});
    var blob=new Blob(out,{type:'audio/ogg'});
    if(blob.size>=f.size)return f;
    return new File([blob],f.name.replace(/\.[^.]+$/,'')+'.opus',{type:'audio/ogg'});
   });
  }
  return AudioEncoder.isConfigSupported({codec:'opus',sampleRate:48000,numberOfChannels:ch,bitrate:SHRINK_KBPS*1000}).then(function(s){
   if(!s.supported)throw new Error('no Opus encoder in this browser');
   var enc=new AudioEncoder({output:function(c){var d=new Uint8Array(c.byteLength);c.copyTo(d);packets.push({d:d,n:Math.round((c.duration||20000)*48/1000)});},error:function(e){err=e;}});
   enc.configure({codec:'opus',sampleRate:48000,numberOfChannels:ch,bitrate:SHRINK_KBPS*1000,opus:{frameDuration:20000,application:'audio'}});
   var CH=48000,chans=[];for(var k=0;k<ch;k++)chans.push(buf.getChannelData(k));
   var i=0;
   function feed(){                                                 // in steps, so the page stays responsive
    var end=Math.min(len,i+CH*10);
    for(;i<end;i+=CH){var n=Math.min(CH,len-i),d=new Float32Array(n*ch);for(var k=0;k<ch;k++)d.set(chans[k].subarray(i,i+n),k*n);
     enc.encode(new AudioData({format:'f32-planar',sampleRate:48000,numberOfFrames:n,numberOfChannels:ch,timestamp:Math.round(i*1e6/48000),data:d}));}
    if(progress)progress(Math.round(i*100/len));
    if(i<len)return new Promise(function(r){setTimeout(r,0);}).then(feed);
    return enc.flush().then(function(){enc.close();if(err)throw err;});
   }
   return feed();
  }).then(function(){
   var PRE=312,serial=(Math.random()*4294967295)>>>0,seq=0,out=[];
   out.push(oggPage([opusHead(ch,PRE)],0,serial,seq++,2));
   var tg=opusTags(comments);                                      // (big tags: split over pages, as Ogg allows)
   for(var at=0;at<tg.length;at+=255*250){var part=tg.subarray(at,Math.min(tg.length,at+255*250));
    out.push(oggPagePart(part,0,serial,seq++,at?1:0,at+255*250<tg.length));}
   var gran=0,page=[],segs=0;
   for(var j=0;j<packets.length;j++){
    var p=packets[j],ns=Math.floor(p.d.length/255)+1;
    if(segs+ns>255||page.length>=50){out.push(oggPage(page,PRE+gran,serial,seq++,0));page=[];segs=0;}
    page.push(p.d);segs+=ns;gran+=p.n;
   }
   out.push(oggPage(page,PRE+Math.min(gran,len),serial,seq++,4));    // the last page: end of stream, exact length
   var blob=new Blob(out,{type:'audio/ogg'});
   if(blob.size>=f.size)return f;                                  // already small: keep it as it was
   return new File([blob],f.name.replace(/\.[^.]+$/,'')+'.opus',{type:'audio/ogg'});
  });
 });
}
// Photo shrinker (Tidy Pro Plus / The Shelf): resizes in the browser and saves as JPEG (TidyOS Pro: its Pictures
// app decodes JPEG on the board) or AVIF (The Shelf). iPhone HEIC photos are read with heic-to, and AVIF is made with
// the jsquash AVIF encoder; both load from the jsDelivr CDN only when needed (nothing is stored on the board).
var PHOTO_CDN={avif:'https://cdn.jsdelivr.net/npm/@jsquash/avif@2.1.1/codec/enc/',heic:'https://cdn.jsdelivr.net/npm/heic-to@1.6.7/dist/heic-to.js'};
var AVIF_OPTS={quality:55,qualityAlpha:-1,denoiseLevel:0,tileColsLog2:0,tileRowsLog2:0,speed:6,subsample:1,chromaDeltaQ:false,sharpness:0,tune:0,enableSharpYUV:false,bitDepth:8,lossless:false};
var avifMod=null;
function avifEncoder(){if(!avifMod)avifMod=import(PHOTO_CDN.avif+'avif_enc.js').then(function(m){return m.default({locateFile:function(p){return PHOTO_CDN.avif+p;}});});return avifMod;}
function isPhoto(f){return /\.(jpe?g|png|webp|heic|heif|bmp|gif|avif)$/i.test(f.name);}
function photoBitmap(f){
 var heic=/\.(heic|heif)$/i.test(f.name);
 var src=heic?import(PHOTO_CDN.heic).then(function(m){return m.heicTo({blob:f,type:'image/jpeg',quality:0.92});}):Promise.resolve(f);
 return src.then(function(b){return createImageBitmap(b,{imageOrientation:'from-image'});});
}
// fmt 'jpeg' or 'avif'; maxPx: the longer side. Resolves to a new File, or the original when that's already smaller.
function shrinkPhoto(f,fmt,maxPx){
 return photoBitmap(f).then(function(img){
  var s=Math.min(1,maxPx/Math.max(img.width,img.height)),w=Math.max(1,Math.round(img.width*s)),h=Math.max(1,Math.round(img.height*s));
  var cv=document.createElement('canvas');cv.width=w;cv.height=h;var g=cv.getContext('2d');
  if(fmt=='jpeg'){g.fillStyle='#fff';g.fillRect(0,0,w,h);}                    // (JPEG has no transparency)
  g.drawImage(img,0,0,w,h);
  var made=fmt=='avif'
   ?avifEncoder().then(function(m){var out=m.encode(new Uint8Array(g.getImageData(0,0,w,h).data.buffer),w,h,AVIF_OPTS);if(!out)throw new Error('AVIF encoding failed');return new Blob([out],{type:'image/avif'});})
     .catch(function(){return new Promise(function(r){cv.toBlob(r,'image/webp',0.8);});})   // no AVIF encoder (offline?): WebP, nearly as small
   :new Promise(function(r){cv.toBlob(r,'image/jpeg',0.82);});
  return made.then(function(b){
   var ext=b.type=='image/avif'?'.avif':b.type=='image/webp'?'.webp':'.jpg';
   var sameKind=(ext=='.jpg'&&/\.jpe?g$/i.test(f.name))||(ext!='.jpg'&&new RegExp('\\'+ext+'$','i').test(f.name));
   if(b.size>=f.size&&sameKind)return f;                                     // already small
   return new File([b],f.name.replace(/\.[^.]+$/,'')+ext,{type:b.type});
  });
 });
}
// dropped files go to the board's Incoming folder (on the SD card, or the board's own storage). On the way, music is
// shrunk to Opus 48 kbps (keeping its tags and cover) and photos to JPEG at 1280 px; hold Alt while dropping to send as they are
var dropDir='/SD/Incoming/',dropRaw=false;
function kb(n){return n>=1048576?(n/1048576).toFixed(1)+' MB':Math.ceil(n/1024)+' KB';}
function dropPrep(f){
 if(dropRaw)return Promise.resolve(f);
 var before=f.size,nm0=f.name;
 var p=canShrink(f)?shrinkAudio(f,function(pc){note('Shrinking '+nm0+' for TidyOS... '+pc+'%');})
      :isPhoto(f)?(note('Shrinking '+nm0+' for TidyOS...'),shrinkPhoto(f,'jpeg',1280)):null;
 if(!p)return Promise.resolve(f);
 return p.then(function(o){if(o!==f)note('Shrunk '+nm0+': '+kb(before)+' to '+kb(o.size));return o;})
  .catch(function(e){note('Couldn\'t shrink '+nm0+' ('+(e&&e.message||e)+') - sending it as it is.',4000);return f;});
}
function dropNext(){var f0=dropQ.shift();if(!f0){return;}
 dropPrep(f0).then(function(f){
 var nm=f.name.replace(/[\/\\:*?"<>|]/g,'_').replace(/[^\x20-\x7e]/g,'_').slice(0,90),tries=0;
 note('Sending '+nm+' ('+Math.ceil(f.size/1024)+' KB)...');
 (function put(){fetch(mUrl('/api/file?path='+encodeURIComponent(dropDir+nm)),{method:'PUT',body:f}).then(function(r){if(!r.ok)throw r.status;
   send([16].concat(Array.from(new TextEncoder().encode(nm))));note('Added '+nm+' to Incoming.',dropQ.length?0:5000);dropNext();})
  .catch(function(e){if(++tries<6&&typeof e!='number'){setTimeout(put,800);return;}   // the board's file server may still be starting
   note('Couldn\'t send '+nm+(typeof e=='number'?' (TidyOS said '+e+')':' (no answer from TidyOS)')+'.',8000);dropNext();});})();});}
function media(t){var k=t.charAt(0),v=t.substr(2),iv=document.getElementById('iv');
 if(k=='F'){relay(v);return;}
 if(midiCur&&(k=='p'||k=='r'||k=='s'||k=='v')){if(k=='p'){midiCur.paused=true;midiCur.ctx.suspend();midiReport();}else if(k=='r'){midiCur.paused=false;midiCur.ctx.resume();}else if(k=='s'){midiStop();send([8,255,0,0,0,0]);}else midiCur.out.gain.value=Math.max(0,Math.min(1,+v));return;}
 if(k=='m'){midiPlay(mUrl(v),au?au.volume:0.8);return;}
 if(k=='o'){var t=v.split('\t');openBanner(mUrl(t[0]),t[1]||'');return;}
 if(k=='g'){store(v);return;}
 if(k=='w'){wxAt=0;weather();nwAt=0;news();return;}
 if(k=='u'){if(v.indexOf('go')==0){var dd=v.split('\t')[1];if(dd)dropDir=dd;setTimeout(dropNext,300);}else if(v.indexOf('cloud\t')==0){var a=v.split('\t');cloudNext(a[1],a[2]);}
  else if(v=='nourl'){dropQ=[];note('Set the Git-Store relay address first.',6000);}
  else{dropQ=[];note('TidyOS has nowhere to keep files right now (no SD card and no board storage).',6000);}return;}
 if(k=='a'){midiStop();if(!au){au=new Audio();au.onended=auReport;setInterval(auReport,1000);
   au.onerror=function(){blog('audio: error '+(au.error?au.error.code+' '+(au.error.message||''):'?')+', try '+au.tries);if(au.tries++<4&&au.getAttribute('src'))setTimeout(function(){au.load();au.play().catch(function(){auBlocked=true;});},1500);};}
  au.tries=0;au.src=mUrl(v);auMeta(v);blog('audio: play '+v);au.play().catch(function(e){blog('audio: play() refused: '+(e&&e.name)+' '+(e&&e.message));if(e&&e.name=='NotAllowedError')auBlocked=true;});}
 else if(k=='p'&&au)au.pause();else if(k=='r'&&au)au.play().catch(function(){auBlocked=true;});
 else if(k=='s'&&au){au.pause();au.removeAttribute('src');au.load();send([8,255,0,0,0,0]);}
 else if(k=='v'&&au)au.volume=Math.max(0,Math.min(1,+v));else if(k=='k'&&au)au.currentTime=+v;
 else if(k=='i'){iv.firstChild.src=mUrl(v);iv.style.display='flex';}else if(k=='x')iv.style.display='none';
 else if(k=='b'){clearInterval(bT);bT=null;alarmBanner(false);if(v=='1'){var n=0;alarmBanner(true);beep();bT=setInterval(function(){beep();if(++n>=120){clearInterval(bT);bT=null;}},1000);}else if(v=='2')beep();}}
addEventListener('pointerdown',function(){unlockSound();if(auBlocked&&au){auBlocked=false;au.play();}if(ac&&ac.state=='suspended')ac.resume();if(midiCur&&midiCur.ctx.state=='suspended'&&!midiCur.paused)midiCur.ctx.resume();},true);
addEventListener('keydown',function(){unlockSound();},true);
function upStr(){if(upBase<0)return '';var u=upBase+Math.floor((Date.now()-upAt)/1000),d=Math.floor(u/86400),h=Math.floor(u/3600)%24,m=Math.floor(u/60)%60,s=u%60;
 return 'Up '+(d?d+'d '+h+'h '+m+'m':h?h+'h '+m+'m':m+'m '+s+'s');}
setInterval(function(){if(ws&&ws.readyState==1)st.textContent='Connected  '+(rx/1024).toFixed(0)+' KB/s'+(upBase>=0?'   \u00b7   '+upStr():'')+(ver?'   \u00b7   v'+ver:'');rx=0;},1000);
// ---- keyboard: browser key codes -> USB HID usages
var K={Enter:40,Escape:41,Backspace:42,Tab:43,Space:44,Minus:45,Equal:46,BracketLeft:47,BracketRight:48,Backslash:49,Semicolon:51,Quote:52,Backquote:53,Comma:54,Period:55,Slash:56,CapsLock:57,
PrintScreen:70,ScrollLock:71,Pause:72,Insert:73,Home:74,PageUp:75,Delete:76,End:77,PageDown:78,ArrowRight:79,ArrowLeft:80,ArrowDown:81,ArrowUp:82,NumLock:83,
NumpadDivide:84,NumpadMultiply:85,NumpadSubtract:86,NumpadAdd:87,NumpadEnter:88,Numpad0:98,NumpadDecimal:99,
ControlLeft:224,ShiftLeft:225,AltLeft:226,MetaLeft:227,ControlRight:228,ShiftRight:229,AltRight:230,MetaRight:231};
for(var i=0;i<26;i++)K['Key'+String.fromCharCode(65+i)]=4+i;
for(var i=1;i<10;i++){K['Digit'+i]=29+i;K['Numpad'+i]=88+i;}K.Digit0=39;
for(var i=1;i<13;i++)K['F'+i]=57+i;
function mods(e){return(e.ctrlKey?1:0)|(e.shiftKey?2:0)|(e.altKey?4:0)|(e.metaKey?8:0);}
addEventListener('keydown',function(e){var u=K[e.code];if(!u||e.keyCode==229)return;e.preventDefault();if(e.repeat)return;down[u]=1;send([1,u,mods(e),1]);});
addEventListener('keyup',function(e){var u=K[e.code];if(!u)return;e.preventDefault();delete down[u];send([1,u,mods(e),0]);});
addEventListener('blur',function(){for(var u in down)send([1,+u,0,0]);down={};send([5]);});
// phone keyboards: typed characters -> key taps
var CH={' ':[44,0],'\n':[40,0],'-':[45,0],'_':[45,2],'=':[46,0],'+':[46,2],'[':[47,0],'{':[47,2],']':[48,0],'}':[48,2],'\\':[49,0],'|':[49,2],';':[51,0],':':[51,2],
"'":[52,0],'"':[52,2],'`':[53,0],'~':[53,2],',':[54,0],'<':[54,2],'.':[55,0],'>':[55,2],'/':[56,0],'?':[56,2],'0':[39,0],')':[39,2]},SH='!@#$%^&*(';
for(var i=0;i<26;i++){CH[String.fromCharCode(97+i)]=[4+i,0];CH[String.fromCharCode(65+i)]=[4+i,2];}
for(var i=1;i<10;i++){CH[''+i]=[29+i,0];CH[SH[i-1]]=[29+i,2];}
function tap(u,m){if(m)send([1,225,2,1]);send([1,u,m,1]);send([1,u,m,0]);if(m)send([1,225,0,0]);}
kb.value=' ';
kb.addEventListener('input',function(e){
 if(e.inputType=='deleteContentBackward')tap(42,0);
 else if(e.inputType=='insertLineBreak')tap(40,0);
 else{var t=e.data||'';for(var i=0;i<t.length;i++){var k=CH[t[i]];if(k)tap(k[0],k[1]);}}
 kb.value=' ';});
kb.addEventListener('keydown',function(e){if(e.key=='Enter'&&e.keyCode==229)tap(40,0);});
document.getElementById('bk').onclick=function(){kb.focus();};
document.getElementById('bt').onclick=function(){send([4]);c.focus();};
document.getElementById('bs').onclick=function(){if(confirm('Put the board into deep sleep? The radio, GPS and screen switch off and it uses almost no power. Press PRG on the board to wake it (or RST), then reload this page.')){send([11]);setTimeout(function(){show('TidyOS Pro is asleep. Press PRG on the board to wake it, then reload.');},600);}c.focus();};
document.getElementById('br').onclick=function(){if(confirm('Restart TidyOS? Anything not saved will be lost.')){send([10]);}c.focus();};
document.getElementById('bz').onclick=function(){mode=mode==0?2:mode==2?1:0;try{localStorage.tidySize=mode;}catch(x){}fit();};
fit();
document.getElementById('bf').onclick=function(){var d=document.documentElement;(d.requestFullscreen||d.webkitRequestFullscreen||function(){}).call(d);};
// ---- mouse and touch
function pos(e){var r=c.getBoundingClientRect();mx=Math.max(0,Math.min(639,(e.clientX-r.left)*640/r.width|0));my=Math.max(0,Math.min(479,(e.clientY-r.top)*480/r.height|0));}
function btns(e){return e.pointerType=='mouse'?(e.buttons&7):(e.type=='pointerup'||e.type=='pointercancel'?0:1);}
// Touch: one finger is the mouse; two fingers pinch to zoom the desktop and drag it around. A finger's press waits
// 80 ms, so the first finger of a pinch doesn't click; a tap shorter than that still clicks.
// Zoomed in, one finger moves the screen too: a swipe pans, a tap clicks, and touch-and-hold (350 ms) then drag
// is the mouse (dragging windows, selecting).
var tp={},tn=0,gz=null,pend=null,tDown=false,pan=null,t0x=0,t0y=0;
function zApply(){c.style.transformOrigin='0 0';c.style.transform=Z>1?'translate('+TX+'px,'+TY+'px) scale('+Z+')':'';document.getElementById('zr').style.display=Z>1?'block':'none';}
function zClamp(){var w=document.getElementById('wrap').getBoundingClientRect(),cw=c.offsetWidth*Z,ch=c.offsetHeight*Z,bx=gz?gz.bx:c.getBoundingClientRect().left-TX,by=gz?gz.by:c.getBoundingClientRect().top-TY;
 TX=cw<=w.width?w.left+(w.width-cw)/2-bx:Math.min(w.left-bx,Math.max(w.right-cw-bx,TX));
 TY=ch<=w.height?w.top+(w.height-ch)/2-by:Math.min(w.top-by,Math.max(w.bottom-ch-by,TY));}
function zReset(){Z=1;TX=TY=0;zApply();fit();}
document.getElementById('zr').onclick=function(){zReset();c.focus();};
function tPts(){var a=[];for(var k in tp)a.push(tp[k]);return a;}
function mSend(){send([2,mx&255,mx>>8,my&255,my>>8,mb,0]);}
c.addEventListener('pointermove',function(e){
 if(e.pointerType=='touch'){if(tp[e.pointerId]){tp[e.pointerId]={x:e.clientX,y:e.clientY};}
  if(gz){var p=tPts();if(p.length<2)return;var d=Math.hypot(p[0].x-p[1].x,p[0].y-p[1].y),mx_=(p[0].x+p[1].x)/2,my_=(p[0].y+p[1].y)/2;
   Z=Math.max(1,Math.min(6,gz.z*d/gz.d));TX=mx_-gz.bx-Z*gz.px;TY=my_-gz.by-Z*gz.py;zClamp();zApply();return;}
  if(pan){TX=pan.tx+e.clientX-pan.x;TY=pan.ty+e.clientY-pan.y;zClamp();zApply();return;}
  if(pend&&Z>1&&Math.hypot(e.clientX-t0x,e.clientY-t0y)>8){clearTimeout(pend);pend=null;pan={x:t0x,y:t0y,tx:TX,ty:TY};
   TX=pan.tx+e.clientX-pan.x;TY=pan.ty+e.clientY-pan.y;zClamp();zApply();return;}   // zoomed in: a swipe moves the screen
  pos(e);if(pend)return;mb=tDown?1:0;mDirty=true;return;}
 pos(e);mb=btns(e);mDirty=true;});
c.addEventListener('pointerdown',function(e){e.preventDefault();c.focus();try{c.setPointerCapture(e.pointerId);}catch(x){}
 if(e.pointerType=='touch'){tp[e.pointerId]={x:e.clientX,y:e.clientY};tn++;
  if(tn==2){                                         // a second finger: zoom, not a click
   if(pend){clearTimeout(pend);pend=null;}
   pan=null;
   if(tDown){mb=0;mDirty=false;mSend();tDown=false;}
   var p=tPts(),r=c.getBoundingClientRect(),bx=r.left-TX,by=r.top-TY,mx_=(p[0].x+p[1].x)/2,my_=(p[0].y+p[1].y)/2;
   gz={d:Math.max(20,Math.hypot(p[0].x-p[1].x,p[0].y-p[1].y)),z:Z,bx:bx,by:by,px:(mx_-bx-TX)/Z,py:(my_-by-TY)/Z};return;}
  if(tn>2)return;
  pan=null;t0x=e.clientX;t0y=e.clientY;
  pos(e);pend=setTimeout(function(){pend=null;if(gz||pan||tn!=1)return;mb=1;mDirty=false;mSend();tDown=true;},Z>1?350:80);return;}
 pos(e);mb=btns(e);mDirty=false;mSend();});
function tEnd(e,cancel){if(!tp[e.pointerId])return;delete tp[e.pointerId];tn=Math.max(0,tn-1);
 if(gz){if(tn<2){gz=null;if(Z<1.08)zReset();else fit();}return;}             // zoom done: draw it sharp at this size
 if(pan){pan=null;return;}                                                     // a swipe that moved the screen
 if(pend){clearTimeout(pend);pend=null;if(!cancel){pos(e);mb=1;mSend();mb=0;mSend();}return;}   // a quick tap
 if(tDown){pos(e);mb=0;mDirty=false;mSend();tDown=false;}}
c.addEventListener('pointerup',function(e){if(e.pointerType=='touch'){tEnd(e,false);return;}pos(e);mb=btns(e);mDirty=false;mSend();});
c.addEventListener('pointercancel',function(e){if(e.pointerType=='touch'){tEnd(e,true);return;}mb=0;mDirty=true;});
document.addEventListener('gesturestart',function(e){e.preventDefault();});   // Safari: no page zoom (it blurs the bar); the desktop zooms itself
document.addEventListener('gesturechange',function(e){e.preventDefault();});
document.addEventListener('dblclick',function(e){if(e.target!=c)e.preventDefault();});
c.addEventListener('wheel',function(e){e.preventDefault();pos(e);send([2,mx&255,mx>>8,my&255,my>>8,mb,e.deltaY>0?1:255]);},{passive:false});
c.addEventListener('contextmenu',function(e){e.preventDefault();});
connect();
</script></body></html>)HTML";

// ---------------------------------------------------------------- WebSocket helpers
static bool readExact(WiFiClient & c, uint8_t * b, size_t n, uint32_t ms = 1000)
{
  uint32_t t0 = millis();
  size_t got = 0;
  while (got < n) {
    int a = c.available();
    if (a > 0) {
      int r = c.read(b + got, std::min((size_t)a, n - got));
      if (r > 0) { got += r; continue; }
    }
    if (!c.connected() || millis() - t0 > ms) return false;
    delay(1);
  }
  return true;
}

static bool wsSend(WiFiClient & c, const uint8_t * d, size_t n, uint8_t opcode = 2)
{
  uint8_t h[4];
  size_t hl;
  h[0] = 0x80 | opcode;
  if (n < 126) { h[1] = n; hl = 2; }
  else { h[1] = 126; h[2] = n >> 8; h[3] = n & 255; hl = 4; }
  if (c.write(h, hl) != hl) return false;
  size_t done = 0;
  while (done < n) {
    size_t w = c.write(d + done, std::min((size_t)1436, n - done));
    if (w == 0) return false;
    done += w;
  }
  gNetTx += n + hl;
  return true;
}

// ---- outbox: one frame for the browser waits here and drains without ever blocking the server.
// A fixed buffer reserved at start-up: no memory is asked for while running (on a busy hotspot
// RAM is too broken up to grow a buffer, even with plenty free in total).
alignas(4) static uint8_t  s_ob[4 + 4 + 16 * 160];   // ws header + frame header + 16 rows (4 colours) / 10 rows (8 colours)
static size_t   s_obLen = 0, s_obPos = 0;
static uint32_t s_outStuck = 0;          // when the outbox last made no progress

static size_t outPending() { return s_obLen - s_obPos; }

// small message (pong, home address); false if the outbox is busy (caller retries later)
static bool wsQueue(const uint8_t * d, size_t n, uint8_t opcode = 2)
{
  if (outPending() || n + 4 > sizeof(s_ob)) return false;
  size_t hl;
  s_ob[0] = 0x80 | opcode;
  if (n < 126) { s_ob[1] = n; hl = 2; }
  else { s_ob[1] = 126; s_ob[2] = n >> 8; s_ob[3] = n & 255; hl = 4; }
  memcpy(s_ob + hl, d, n);
  s_obLen = hl + n; s_obPos = 0;
  gNetTx += s_obLen;
  return true;
}

// false = the connection is gone (or stuck for 15 s)
static bool flushOut(WiFiClient & c)
{
  int fd = c.fd();
  if (fd < 0) return false;
  while (outPending()) {
    int r = send(fd, s_ob + s_obPos, std::min(outPending(), (size_t)2920), MSG_DONTWAIT);
    if (r > 0) { s_obPos += r; s_outStuck = 0; continue; }
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOMEM)) {
      if (!s_outStuck) s_outStuck = millis();
      else if (millis() - s_outStuck > 15000) return false;
      break;
    }
    return false;
  }
  if (!outPending()) { s_obLen = s_obPos = 0; }
  return true;
}

static void sendPalette(WiFiClient & c)
{
  uint8_t m[3 + 8 * 3];
  int n = gColors;
  m[0] = 1; m[1] = gColors == 8 ? 3 : gColors == 2 ? 1 : 2; m[2] = n;
  for (int i = 0; i < n; ++i) { RGB888 p = themePal(i); m[3 + i * 3] = p.R; m[4 + i * 3] = p.G; m[5 + i * 3] = p.B; }
  wsSend(c, m, 3 + n * 3);
}

static inline uint32_t rowHash(const uint8_t * row, int bytes)
{
  const uint32_t * w = (const uint32_t *)row;
  uint32_t h = 2166136261u;
  for (int i = 0; i < bytes / 4; ++i) h = (h ^ w[i]) * 16777619u;
  return h | 1;      // never 0, so 0 can mean "not sent yet"
}

// puts the changed parts of rows into the outbox, as many as fit; returns its size, 0 = nothing changed.
// Each row is compared with what the browser already has (s_prev), and only the stretch from the first to the
// last changed byte is sent: a game's copter is ~40 pixels wide, so that's ~10 bytes of a 160-byte row. (Whole
// rows used to go: ~100 of them every frame in a fast game, more than Wi-Fi could carry, so games looked jerky.)
// The search goes on from where the last batch ended and wraps round, so every changed row gets its turn.
// Message 3: [3][count lo][count hi] then per piece [y lo][y hi][first byte][length][bytes].
static int s_scanY = 0;
static size_t queueChanges()
{
  if (outPending()) return 0;
  const int rb = gColors == 8 ? 240 : gColors == 2 ? 80 : 160;
  // What the browser has: a copy of every row (PSRAM), so only the changed part of a row is sent. A board
  // without PSRAM (the V3) can't spare that much, so it keeps a fingerprint of each row instead and sends
  // a changed row whole.
  static uint32_t * s_rowHash = nullptr;
  if (!s_prev && !s_rowHash) {
    s_prev = (uint8_t *)heap_caps_malloc(SCREEN_H * 240, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_prev) s_rowHash = (uint32_t *)calloc(SCREEN_H, sizeof(uint32_t));
    if (!s_prev && !s_rowHash) return 0;
    memset(s_rowOk, 0, sizeof(s_rowOk));
  }
  const size_t cap = sizeof(s_ob) - 4;
  uint8_t * pay = s_ob + 4;
  size_t pl = 3; int cnt = 0, lastY = -1;
  static uint8_t tmp[240];
  for (int k = 0; k < SCREEN_H; ++k) {
    int y = (s_scanY + k) % SCREEN_H;
    const uint8_t * live = (const uint8_t *)fabgl::VGAPalettedController::sgetScanline(y);
    if (!s_prev) {                                 // fingerprint mode (no PSRAM)
      uint32_t h = 2166136261u;
      for (int i = 0; i < rb; ++i) h = (h ^ live[i]) * 16777619u;
      if (s_rowOk[y] && h == s_rowHash[y]) continue;
      if (pl + 4 + rb > cap) break;
      memcpy(tmp, live, rb);
      h = 2166136261u;
      for (int i = 0; i < rb; ++i) h = (h ^ tmp[i]) * 16777619u;   // (of what's actually sent)
      pay[pl++] = y & 255; pay[pl++] = y >> 8; pay[pl++] = 0; pay[pl++] = (uint8_t)rb;
      memcpy(pay + pl, tmp, rb); pl += rb;
      s_rowHash[y] = h; s_rowOk[y] = true;
      ++cnt; lastY = y;
      continue;
    }
    uint8_t * prev = s_prev + y * 240;
    if (s_rowOk[y] && !memcmp(live, prev, rb)) continue;
    // copy first, then compare the copy: the other core may be drawing into this row right now, and the
    // browser must be known to have exactly what was sent
    memcpy(tmp, live, rb);
    int a = 0, b = rb - 1;
    if (s_rowOk[y]) {
      while (a < rb && tmp[a] == prev[a]) ++a;
      if (a == rb) continue;                       // (it changed back while being copied)
      while (b > a && tmp[b] == prev[b]) --b;
      if (gColors == 8) { a = a / 3 * 3; b = b / 3 * 3 + 2; }   // 8 colours: 3 bytes hold 8 pixels
    }
    int len = b - a + 1;
    if (pl + 4 + len > cap) break;                 // full: the rest next time
    pay[pl++] = y & 255; pay[pl++] = y >> 8; pay[pl++] = (uint8_t)a; pay[pl++] = (uint8_t)len;
    memcpy(pay + pl, tmp + a, len); pl += len;
    memcpy(prev + a, tmp + a, len);
    s_rowOk[y] = true;
    ++cnt; lastY = y;
  }
  if (!cnt) return 0;
  s_scanY = (lastY + 1) % SCREEN_H;                // next time, carry on from here
  pay[0] = 3; pay[1] = cnt & 255; pay[2] = cnt >> 8;
  if (pl >= 126) {                                 // WebSocket: 2-byte length form
    s_ob[0] = 0x82; s_ob[1] = 126; s_ob[2] = pl >> 8; s_ob[3] = pl & 255;
    s_obPos = 0;
  } else {                                         // a small one: the short length form is required,
    s_ob[2] = 0x82; s_ob[3] = pl;                  // or browsers drop us
    s_obPos = 2;
  }
  s_obLen = 4 + pl;
  gNetTx += s_obLen - s_obPos;
  return s_obLen;
}

// browser -> TidyOS
// files fetched by the page for us (screenRelayGet)
static StreamBufferHandle_t s_rlBuf = nullptr;
static volatile bool s_rlOn = false;
static volatile int  s_rlStatus = -1, s_rlEnd = 0;
static volatile uint32_t s_rlTotal = 0;

volatile bool gMusicNextReq = false;     // the page's media controls asked for the next song

static bool pollInput(WiFiClient & c)
{
  while (c.available() >= 2) {
    uint8_t h[2];
    if (!readExact(c, h, 2)) return false;
    uint8_t op = h[0] & 15;
    size_t len = h[1] & 127;
    if (len == 126) { uint8_t e[2]; if (!readExact(c, e, 2)) return false; len = (e[0] << 8) | e[1]; }
    else if (len == 127) return false;
    uint8_t mask[4] = { 0, 0, 0, 0 };
    if ((h[1] & 128) && !readExact(c, mask, 4)) return false;
    static uint8_t p[4200];                        // replies from the page (Git-Store lists) can be long; relayed files come in 4000-byte pieces
    size_t keep = std::min(len, sizeof(p));
    if (!readExact(c, p, keep)) return false;
    for (size_t i = keep; i < len; ++i) { uint8_t junk; if (!readExact(c, &junk, 1)) return false; }
    for (size_t i = 0; i < keep; ++i) p[i] ^= mask[i & 3];
    gNetRx += len + 2;
    if (op == 8) return false;                               // close
    if (op == 9) { wsQueue(p, keep, 10); continue; }         // ping -> pong
    if (op != 2 || keep < 1) continue;
    switch (p[0]) {
      case 1: if (keep >= 4) inputWebKey(p[1], p[2], p[3] != 0); break;
      case 2: if (keep >= 7) inputWebMouse(p[1] | (p[2] << 8), p[3] | (p[4] << 8), (int8_t)p[6], p[5]); break;
      case 4: gOpenTaskMgr = true; break;
      case 10: gRestartReq = true; break;
      case 11: gSleepReq = true; break;
      case 12: { char t[65]; size_t n = std::min(keep - 1, sizeof(t) - 1); memcpy(t, p + 1, n); t[n] = 0; deskWeatherFromPage(t); break; }   // weather, looked up by the page
      case 13: { p[keep < sizeof(p) ? keep : sizeof(p) - 1] = 0; storeFromPage((const char *)p + 1); break; }   // Git-Store reply
      case 15: deskDropStart(keep >= 5 ? p[1] | (p[2] << 8) : -1, keep >= 5 ? p[3] | (p[4] << 8) : -1); break;                                                                  // files dropped on the page
      case 16: { p[keep < sizeof(p) ? keep : sizeof(p) - 1] = 0; deskDropped((const char *)p + 1); break; }
      case 18: { p[keep < sizeof(p) ? keep : sizeof(p) - 1] = 0; deskCityTempsFromPage((const char *)p + 1); break; } // Moscow, LA, Tokyo
      case 17: { p[keep < sizeof(p) ? keep : sizeof(p) - 1] = 0; deskTempsFromPage((const char *)p + 1); break; } // temperatures, last 4 days
      case 19: { p[keep < sizeof(p) ? keep : sizeof(p) - 1] = 0; deskFieldFromPage((const char *)p + 1); break; } // TidyOS Pro field conditions
      case 14: { p[keep < sizeof(p) ? keep : sizeof(p) - 1] = 0; deskNewsFromPage((const char *)p + 1); break; } // news headlines
      case 25: gMusicNextReq = true; break;                    // Next from the phone's or computer's media controls
      case 22:                                                  // a piece of a file the page fetched for us
        if (s_rlOn && s_rlBuf) {
          size_t off = 1;
          while (off < keep && s_rlOn) off += xStreamBufferSend(s_rlBuf, p + off, keep - off, pdMS_TO_TICKS(200));
        }
        break;
      case 23: if (keep >= 7) { s_rlTotal = p[3] | (p[4] << 8) | (p[5] << 16) | ((uint32_t)p[6] << 24); s_rlStatus = p[1] | (p[2] << 8); } break;
      case 24: s_rlEnd = keep >= 2 && p[1] ? 1 : 2; break;
      case 21: { p[keep < sizeof(p) ? keep : sizeof(p) - 1] = 0; Serial.printf("[browser] %s\n", (const char *)p + 1); break; }   // the page's own notes (music errors)
      case 5: inputWebReleaseAll(); break;
      case 6: if (keep >= 5) netSetClockFrom(p[1] | (p[2] << 8) | (p[3] << 16) | ((uint32_t)p[4] << 24)); break;
      case 8: if (keep >= 6) { s_mState = p[1] == 255 ? -1 : p[1]; s_mPos = p[2] | (p[3] << 8); s_mDur = p[4] | (p[5] << 8); } break;
    }
  }
  return true;
}

// ---------------------------------------------------------------- HTTP
static void dropViewer()
{
  s_obLen = s_obPos = 0; s_outStuck = 0;
  if (s_view) { s_view = false; inputWebReleaseAll(); }
  s_ws.stop();
}

// parses a complete request header (already read) and answers it
static void handleHttp(WiFiClient c, const std::string & req)
{
  std::string key, path;
  bool upgrade = false, first = true;
  size_t st = 0;
  while (st < req.size()) {
    size_t e = req.find('\n', st);
    if (e == std::string::npos) e = req.size();
    std::string line = req.substr(st, e - st);
    st = e + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    if (first) {
      first = false;
      size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 + 1);
      if (sp1 != std::string::npos) path = line.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);
    } else {
      std::string low = line;
      for (auto & k : low) k = tolower(k);
      if (low.rfind("sec-websocket-key:", 0) == 0) { key = line.substr(18); while (!key.empty() && key[0] == ' ') key.erase(0, 1); }
      if (low.rfind("upgrade:", 0) == 0 && low.find("websocket") != std::string::npos) upgrade = true;
    }
  }
  c.setTimeout(2);
  gNetRx += 300;

  if (path == "/ws" && upgrade && !key.empty()) {
    std::string src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char sha[20], b64[40];
    size_t olen = 0;
    mbedtls_sha1_ret((const unsigned char *)src.data(), src.size(), sha);
    mbedtls_base64_encode(b64, sizeof(b64), &olen, sha, 20);
    std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
    resp.append((const char *)b64, olen);
    resp += "\r\n\r\n";
    if (s_view) { uint8_t k = 3; wsSend(s_ws, &k, 1); s_ws.flush(); delay(40); dropViewer(); }     // tell the old browser it was taken over (and let that message get there before the socket closes, or it just reconnects and the two pages take turns)
    c.write((const uint8_t *)resp.data(), resp.size());
    c.setNoDelay(true);
    s_ws = c;
    memset(s_rowOk, 0, sizeof(s_rowOk)); s_scanY = 0;
    s_obLen = s_obPos = 0; s_outStuck = 0;
    sendPalette(s_ws);
    {   // version + uptime for the page's top bar (the page keeps counting from here)
      char m[48];
      int n = snprintf(m, sizeof(m), "\x08" TIDYOS_VERSION "\n%lu", (unsigned long)(millis() / 1000));
      if (n > 0 && n < (int)sizeof(m)) wsSend(s_ws, (const uint8_t *)m, n);
    }
    s_sentHome[0] = 0;
    s_view = true;
    Serial.println("[TidyOS] browser connected");
    s_mState = -1; s_mEpoch++;
    return;
  }
  std::string p = path.substr(0, path.find('?'));
  // "Devices reach TidyOS" hotspot: the "is there internet?" checks get the answers that mean yes
  const char * ok = nullptr, * type = "text/plain";
  int code = 200;
  if (s_netMode) {
    if (p == "/generate_204" || p == "/gen_204") code = 204, ok = "";                                     // Android, Chrome
    else if (p == "/hotspot-detect.html" || p == "/library/test/success.html" || (p.size() > 5 && p.compare(p.size() - 5, 5, ".html") == 0 && req.find("CaptiveNetworkSupport") != std::string::npos))
      ok = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>", type = "text/html";   // Apple
    else if (p == "/connecttest.txt") ok = "Microsoft Connect Test";                                    // Windows 10 / 11
    else if (p == "/ncsi.txt") ok = "Microsoft NCSI";                                                   // older Windows
    else if (p == "/success.txt" || p == "/canonical.html") ok = "success\n";                           // Firefox
    else if (p == "/check_network_status.txt") ok = "NetworkManager is online\n";                       // Linux
  }
  if (ok) {
    char h[160];
    snprintf(h, sizeof(h), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
             code, code == 204 ? "No Content" : "OK", type, (unsigned)strlen(ok));
    c.print(h);
    if (*ok) c.print(ok);
  } else if (p == "/favicon.ico") {
    c.print("HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  } else if (p == "/files" || p == "/files/") {
    // the file page (SD card + the board's storage, port 8081): start it for now (it stops by itself after 10 idle
    // minutes unless Wi-Fi sharing is on in Settings) and send the browser there, on the address it used to reach us
    fileServerMediaReady();
    char r[200];
    snprintf(r, sizeof(r), "HTTP/1.0 302 Found\r\nLocation: http://%s:%d/\r\nCache-Control: no-cache\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
             c.localIP().toString().c_str(), fileServerPort());
    c.print(r);
  } else if (p != "/" && p != "/index.html") {
    // phones' "is there internet?" checks and anything else: a tiny redirect to the desktop
    // (makes the phone offer to open it, without sending the whole page to every check)
    std::string host = s_ap && WiFi.status() != WL_CONNECTED ? WiFi.softAPIP().toString().c_str() : WiFi.localIP().toString().c_str();
    std::string r = "HTTP/1.0 302 Found\r\nLocation: http://" + host + "/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    c.write((const uint8_t *)r.data(), r.size());
  } else {
    // the desktop page
    char h[160];
    snprintf(h, sizeof(h), "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n", (unsigned)(sizeof(PAGE) - 1));
    c.print(h);
    for (size_t i = 0; i < sizeof(PAGE) - 1; i += 1400) c.write((const uint8_t *)PAGE + i, std::min((size_t)1400, sizeof(PAGE) - 1 - i));
    gNetTx += sizeof(PAGE);
  }
  delay(5);
  c.stop();
}

// ---------------------------------------------------------------- setup hotspot
static bool s_captive = false;      // the hotspot is a sign-in page (first setup only)

// ---- "Devices reach TidyOS" hotspot (the default): the hotspot hands out itself as gateway and DNS server and
// answers Windows', Apple's and Android's internet checks the way they expect. The devices then treat the hotspot as
// their network and send everything there, so http://10.1.1.250 opens in any browser, even with mobile data on
// (with no gateway they mark it "no internet" and Android, Windows and iPhones send the browser over cellular instead).
// Only the check addresses and TidyOS's own names get an answer; every other name is "doesn't exist", so phones'
// background traffic fails at once instead of filling the board's four connection slots.
static WiFiUDP s_udns;
static bool    s_udnsOn = false;
static bool dnsKnown(const char * n, uint32_t & ip)
{
  static const char * const names[] = {
    // Android (Google, Samsung, Xiaomi, Huawei)
    "connectivitycheck.gstatic.com", "connectivitycheck.android.com", "clients1.google.com", "clients3.google.com",
    "play.googleapis.com", "www.google.com", "connect.rom.miui.com", "connectivitycheck.platform.hicloud.com",
    // (not Apple's check names: an iPhone whose check fails keeps the hotspot for http://10.1.1.250 and sends
    //  everything else over cellular, so the page can fetch the weather, news and updates for TidyOS. If its check
    //  passed, it would send all its internet to the board, which has none. Before, this only worked when the
    //  phone joined in the first moments after a start, before the board was answering.)
    // Windows
    "www.msftconnecttest.com", "www.msftncsi.com", "ipv6.msftconnecttest.com",
    // Firefox, Linux
    "detectportal.firefox.com", "nmcheck.gnome.org", "connectivity-check.ubuntu.com",
    // TidyOS's own names: http://tidyos or http://tidy also open the desktop on the hotspot
    "tidyos", "tidy", "tidyos.lan", "tidyos.local",
  };
  ip = (uint32_t)WiFi.softAPIP();
  if (!strcmp(n, "dns.msftncsi.com")) { ip = (uint32_t)IPAddress(131, 107, 255, 255); return true; }   // Windows' DNS check wants exactly this
  for (auto k : names) if (!strcmp(n, k)) return true;
  static std::string host;
  if (host.empty()) { host = boardHost(); for (auto & c : host) c = tolower(c); }
  if (!strcmp(n, host.c_str())) return true;
  return host.size() && !strncmp(n, host.c_str(), host.size()) && (!strcmp(n + host.size(), ".local") || !strcmp(n + host.size(), ".lan"));
}
static void dnsPoll()
{
  for (int k = 0; k < 4; ++k) {
    int len = s_udns.parsePacket();
    if (len <= 0) return;
    static uint8_t q[512];
    if (len > (int)sizeof(q)) { s_udns.flush(); continue; }
    len = s_udns.read(q, len);
    if (len < 17 || (q[2] & 0x80) || q[4] != 0 || q[5] != 1) continue;   // one question, and a query
    char name[128]; int ni = 0, p = 12;
    while (p < len && q[p]) {
      int l = q[p++];
      if (l > 63 || p + l > len || ni + l + 1 >= (int)sizeof(name)) { ni = -1; break; }
      if (ni) name[ni++] = '.';
      for (int i = 0; i < l; ++i) name[ni++] = tolower(q[p + i]);
      p += l;
    }
    if (ni < 0 || p + 5 > len) continue;
    name[ni] = 0;
    p++;                                                   // the 0 at the end of the name
    uint16_t qtype = q[p] << 8 | q[p + 1];
    int qend = p + 4;
    uint32_t ip;
    bool known = dnsKnown(name, ip);
    static uint8_t r[512 + 16];
    memcpy(r, q, qend);
    r[2] = 0x84 | (q[2] & 0x01);                           // response, authoritative, copy "recursion desired"
    r[3] = 0x80 | (known ? 0 : 3);                         // recursion available; "no such name" for the rest
    r[6] = r[7] = r[8] = r[9] = r[10] = r[11] = 0;
    int n = qend;
    if (known && qtype == 1) {                             // an A record; other types (IPv6...): none, but no error
      r[7] = 1;
      const uint8_t a[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4 };
      memcpy(r + n, a, sizeof(a)); n += sizeof(a);
      memcpy(r + n, &ip, 4); n += 4;
    }
    s_udns.beginPacket(s_udns.remoteIP(), s_udns.remotePort());
    s_udns.write(r, n);
    s_udns.endPacket();
  }
}

// What the hotspot's DHCP server hands out. Needs the hotspot up.
static void apNetConfig()
{
  esp_netif_t * ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap) return;
  s_netMode = !s_captive && !gSet.hotspotCell;
  esp_netif_dhcps_stop(ap);
  dhcps_offer_t give = (s_captive || s_netMode) ? 1 : 0;   // captive / "devices reach TidyOS": gateway + DNS = the board
  esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &give, sizeof(give));
  esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &give, sizeof(give));
  if (give) {
    esp_netif_dns_info_t d = {};
    d.ip.type = ESP_IPADDR_TYPE_V4;
    d.ip.u_addr.ip4.addr = (uint32_t)WiFi.softAPIP();
    esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &d);
  }
  esp_netif_dhcps_start(ap);
  if (s_netMode && !s_udnsOn) s_udnsOn = s_udns.begin(53);
  if (!s_netMode && s_udnsOn) { s_udns.stop(); s_udnsOn = false; }
  Serial.printf("[TidyOS] hotspot: %s\n", s_captive ? "sign-in page (first setup)" :
                s_netMode ? "devices treat it as their network (10.1.1.250 always opens; no phone internet meanwhile)"
                          : "phones keep their own internet (Android may need \"Stay connected\" or mobile data off)");
}
static bool s_staPaused = false;    // not looking for home Wi-Fi while devices use the hotspot
// While a phone or laptop is on the hotspot and home Wi-Fi isn't in range, stop looking for it: each
// search hops the radio across channels, and the hotspot's devices drop off. Look again once they've gone.
static void staPause(bool on)
{
  if (on == s_staPaused) return;
  s_staPaused = on;
  if (on) {
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false);          // (false: the hotspot stays up)
    Serial.println("[TidyOS] hotspot in use: not looking for home Wi-Fi meanwhile");
  } else if (!gSet.hotspot) {
    WiFi.setAutoReconnect(true);
    if (gSet.ssid.length()) WiFi.begin(gSet.ssid.c_str(), gSet.pass.c_str());
    Serial.println("[TidyOS] looking for home Wi-Fi again");
  }
}
bool screenStaPaused() { return s_staPaused; }

static void apStart()
{
  if (s_ap) return;
  static bool logged = false;
  if (!logged) {
    logged = true;
    WiFi.onEvent([](arduino_event_id_t e, arduino_event_info_t info) {
      if (e == ARDUINO_EVENT_WIFI_AP_STACONNECTED)
        Serial.printf("[TidyOS] hotspot: device %02x:%02x:%02x:%02x:%02x:%02x joined\n", info.wifi_ap_staconnected.mac[0], info.wifi_ap_staconnected.mac[1],
                      info.wifi_ap_staconnected.mac[2], info.wifi_ap_staconnected.mac[3], info.wifi_ap_staconnected.mac[4], info.wifi_ap_staconnected.mac[5]);
      else if (e == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) Serial.println("[TidyOS] hotspot: a device left");
      else if (e == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED)   Serial.printf("[TidyOS] hotspot: gave out %s\n", IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
    });
  }
  WiFi.mode(WIFI_AP_STA);
  // up to 4 devices; lower transmit power so the current peaks don't dip the 3.3 V supply
  // (the hotspot only needs to reach across a room)
  static std::string apName = boardId();
  // channel 1, the same as The Shelf's own Wi-Fi: the Coms mesh (ESP-NOW) only reaches boards on the same channel
  // (on home Wi-Fi the hotspot follows the router's channel anyway)
  WiFi.softAP(apName.c_str(), SCREEN_AP_PASS, WiFi.status() == WL_CONNECTED ? WiFi.channel() : 1, 0, 4);
  // a fixed address, the same as The Shelf's own Wi-Fi: http://10.1.1.250 (phones get 10.1.1.100 and up)
  WiFi.softAPConfig(IPAddress(10, 1, 1, 250), IPAddress(10, 1, 1, 250), IPAddress(255, 255, 255, 0), IPAddress(10, 1, 1, 100));
  // 15 dBm: reaches a laptop across the room or the next one (11 dBm was too weak for some Windows PCs),
  // still well short of the peaks that can dip the 3.3 V supply
  WiFi.setTxPower(WIFI_POWER_15dBm);
  WiFi.setSleep(false);
  // plain WPA2 (this IDF's hotspot has no PMF), the usual beacon timing: what Windows drivers and older phones join best
  {
    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK) {
      cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
      cfg.ap.beacon_interval = 100;
      esp_wifi_set_config(WIFI_IF_AP, &cfg);
    }
  }
  delay(100);
  Serial.printf("[TidyOS] hotspot up on channel %d, free RAM %u\n", WiFi.channel(), (unsigned)ramFree());
  // First setup (no Wi-Fi saved yet): a sign-in page, so the phone pops TidyOS up by itself.
  // Otherwise (Switch in Settings > Run TidyOS Hotspot): either the devices treat the hotspot as their network
  // (the default: 10.1.1.250 always opens), or, as before, no gateway and no DNS so phones keep using cellular.
  s_captive = !gSet.ssid.length();
  if (s_captive) {
    s_dns.setErrorReplyCode(DNSReplyCode::NoError);
    s_dns.start(53, "*", WiFi.softAPIP());
  }
  apNetConfig();
  s_ap = true;
  s_apSince = millis();
  Serial.printf("[TidyOS] setup hotspot \"%s\" (password %s) - open http://%s\n", SCREEN_AP_NAME, SCREEN_AP_PASS, WiFi.softAPIP().toString().c_str());
}

static void apStop()
{
  if (!s_ap) return;
  if (s_captive) s_dns.stop();
  s_captive = false;
  if (s_udnsOn) { s_udns.stop(); s_udnsOn = false; }
  s_netMode = false;
  staPause(false);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  s_ap = false;
  delay(200);
  netFixDns();
  Serial.println("[TidyOS] setup hotspot off - on home Wi-Fi now");
}

static void apManage()
{
  bool sta = WiFi.status() == WL_CONNECTED;
  uint32_t now = millis();
  if (sta) { if (!s_connSince) s_connSince = now; } else s_connSince = 0;
  // no Wi-Fi set up, or it hasn't connected 25 s after start: open the hotspot
  if (!s_ap && !sta && (!gSet.ssid.length() || now > 25000)) apStart();
  if (!s_ap && gSet.hotspot) apStart();                         // "Run TidyOS Hotspot" in Settings
  {                                                             // devices on the hotspot, no home Wi-Fi: stop searching
    static uint32_t emptySince = 0;
    int n = s_ap ? WiFi.softAPgetStationNum() : 0;
    if (n > 0) emptySince = 0; else if (!emptySince) emptySince = now;
    // (soon after a device joins: each search for home Wi-Fi hops channels, and a PC or phone still joining gives up)
    if (s_ap && !sta && n > 0 && gSet.ssid.length() && now - s_apSince > 5000) staPause(true);
    else if (s_staPaused && (n == 0 && now - emptySince > 30000)) staPause(false);   // all gone for 30 s: look again
  }
  // on home Wi-Fi for 2 minutes and nobody uses the hotspot: close it
  if (s_ap && !gSet.hotspot && sta && s_connSince && now - s_connSince > 300000 && WiFi.softAPgetStationNum() == 0) apStop();
}

// Several connections at once, none allowed to stall the others: browsers and Windows open
// connections early and may say nothing for a while. Each one's request is collected bit by bit;
// silent ones are dropped after 4 s, and the oldest is dropped if all slots are busy.
struct Pending { WiFiClient c; std::string buf; uint32_t t0 = 0; bool used = false; };
static Pending s_pend[4];

static void pollHttp()
{
  for (int n = 0; n < 4; ++n) {              // accept what's waiting
    WiFiClient c = s_srv.available();
    if (!c) break;
    int slot = -1;
    uint32_t oldest = 0xFFFFFFFF;
    for (int i = 0; i < 4; ++i) {
      if (!s_pend[i].used) { slot = i; break; }
      if (s_pend[i].t0 < oldest) { oldest = s_pend[i].t0; slot = i; }
    }
    if (s_pend[slot].used) s_pend[slot].c.stop();
    s_pend[slot].c = c;
    s_pend[slot].buf.clear();
    s_pend[slot].t0 = millis();
    s_pend[slot].used = true;
  }
  for (auto & p : s_pend) {
    if (!p.used) continue;
    int a = p.c.available();
    if (a > 0) {
      uint8_t tmp[256];
      int r = p.c.read(tmp, std::min(a, (int)sizeof(tmp)));
      if (r > 0 && p.buf.size() < 2048) p.buf.append((const char *)tmp, r);
      gNetRx += r > 0 ? r : 0;
    }
    bool done = p.buf.find("\r\n\r\n") != std::string::npos || p.buf.find("\n\n") != std::string::npos;
    if (done) {
      WiFiClient c = p.c;
      std::string req;
      req.swap(p.buf);
      p.used = false;
      p.c = WiFiClient();
      handleHttp(c, req);
    } else if (!p.c.connected() || millis() - p.t0 > 4000 || p.buf.size() >= 2048) {
      p.c.stop();
      p.used = false;
      p.buf.clear();
      p.buf.shrink_to_fit();
    }
  }
}

// ---------------------------------------------------------------- task
static void screenTask(void *)
{
  uint32_t lastFrame = 0, lastAp = 0;
  for (;;) {
    uint32_t now = millis();
    if (now - lastAp > 1000) {
      lastAp = now;
      apManage();
      if (WiFi.status() == WL_CONNECTED) netMdnsBegin();
      if (!s_listening && (s_ap || WiFi.status() == WL_CONNECTED)) { s_srv.begin(); s_srv.setNoDelay(true); s_listening = true; }
    }
    if (s_ap && s_captive) for (int i = 0; i < 4; ++i) s_dns.processNextRequest();   // answer a burst of lookups promptly
    if (s_ap && s_udnsOn) dnsPoll();
    if (s_listening) pollHttp();
    if (s_view) {
      if (!s_ws.connected() || !pollInput(s_ws)) { dropViewer(); Serial.println("[TidyOS] browser left"); }
      else if (now - lastAp == 0 && WiFi.status() == WL_CONNECTED) {
        // tell the page where TidyOS lives on the home Wi-Fi (it shows this if it's on the hotspot)
        // (fixed buffers: nothing is allocated here, it runs every second)
        static char m[80];
        static char hn[24] = "";
        if (!hn[0]) strlcpy(hn, boardHost().c_str(), sizeof(hn));
        IPAddress ip = WiFi.localIP();
        wifi_ap_record_t ap;
        const char * ssid = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? (const char *)ap.ssid : "";
        int n = snprintf(m, sizeof(m), "\x07%.32s\n%u.%u.%u.%u\n%s", ssid, ip[0], ip[1], ip[2], ip[3], hn);
        if (n > 0 && n < (int)sizeof(m) && strcmp(m + 1, s_sentHome) != 0 && wsQueue((const uint8_t *)m, n))
          strncpy(s_sentHome, m + 1, sizeof(s_sentHome) - 1);
      }
      while (s_view && s_mqR != s_mqW && outPending() == 0) {
        static uint8_t mm[224];
        const char * cmd = s_mediaQ[s_mqR % 4];
        size_t n = strlen(cmd);
        mm[0] = 9; memcpy(mm + 1, cmd, n);
        if (!wsQueue(mm, n + 1)) break;
        s_mqR++;
        if (!flushOut(s_ws)) break;
      }
      if (s_view && !flushOut(s_ws)) { dropViewer(); Serial.println("[TidyOS] browser stopped receiving"); }
      else if (s_view && now - lastFrame >= 50 && outPending() == 0) {
        lastFrame = now;
        size_t budget = 0;
        while (s_view && budget < 8000 && outPending() == 0) {
          size_t n = queueChanges();
          if (!n) break;
          budget += n;
          if (!flushOut(s_ws)) { dropViewer(); break; }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(s_view ? 8 : 20));
  }
}

void screenBegin()
{
  static bool started = false;
  if (started) return;
  started = true;
  xTaskCreatePinnedToCore(screenTask, "screen", 12288, nullptr, 2, nullptr, 0);
}

bool screenViewer() { return s_view; }

// Gets a file over the internet of the device showing TidyOS's screen (see relay() in the page). Calls sink
// with each piece and the total size; returns the HTTP status (200 = all of it came), or below 0 if the page
// couldn't (no page open, no answer, it stopped part way). For a task other than the screen's own.
int screenRelayGet(const char * url, const std::function<bool(const uint8_t *, size_t, size_t)> & sink)
{
  if (!s_view) return -10;
  if (!s_rlBuf) s_rlBuf = xStreamBufferCreate(32768, 1);
  if (!s_rlBuf) return -15;
  s_rlOn = false;
  xStreamBufferReset(s_rlBuf);
  s_rlStatus = -1; s_rlTotal = 0; s_rlEnd = 0;
  s_rlOn = true;
  screenMedia(std::string("F ") + url);
  uint32_t t0 = millis();
  while (s_rlStatus < 0 && s_view && millis() - t0 < 25000) delay(20);
  if (s_rlStatus != 200) { int st = s_rlStatus; s_rlOn = false; Serial.printf("[TidyOS] browser fetch of %s: %d\n", url, st); return st < 0 ? -11 : st == 0 ? -16 : st; }
  static uint8_t buf[2048];
  uint32_t last = millis();
  for (;;) {
    size_t n = xStreamBufferReceive(s_rlBuf, buf, sizeof(buf), pdMS_TO_TICKS(100));
    if (n) { last = millis(); if (!sink(buf, n, s_rlTotal)) { s_rlOn = false; return -12; } continue; }
    if (s_rlEnd && xStreamBufferIsEmpty(s_rlBuf)) break;
    if (!s_view || millis() - last > 30000) { s_rlOn = false; return -13; }
  }
  s_rlOn = false;
  return s_rlEnd == 1 ? 200 : -14;
}

void screenMedia(const std::string & cmd)
{
  portENTER_CRITICAL(&s_mqMux);
  if ((uint8_t)(s_mqW - s_mqR) >= 4) s_mqR++;           // full: drop the oldest
  strlcpy(s_mediaQ[s_mqW % 4], cmd.c_str(), sizeof(s_mediaQ[0]));
  s_mqW++;
  portEXIT_CRITICAL(&s_mqMux);
}

void screenMediaState(int & state, int & pos, int & dur) { state = s_mState; pos = s_mPos; dur = s_mDur; }
int  screenMediaEpoch() { return s_mEpoch; }
bool screenApOn()   { return s_ap; }
void screenTrim()            // the Optimize button: let go of what isn't being used
{
  if (s_ap && !gSet.hotspot && WiFi.status() == WL_CONNECTED && WiFi.softAPgetStationNum() == 0) apStop();
  for (auto & p : s_pend) if (!p.used) { p.c.stop(); std::string().swap(p.buf); }
}
// "Run TidyOS Hotspot": TidyOS leaves the home Wi-Fi and runs only its own network (devices join it
// directly). Off again: back to the saved Wi-Fi; the hotspot closes once that's joined.
void screenHotspotMode() { if (s_ap) apNetConfig(); }
void screenHotspot(bool on)
{
  gSet.hotspot = on;
  if (on) {
    apStart();
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false);              // (false: keep the hotspot up)
  } else {
    WiFi.setAutoReconnect(true);
    if (gSet.ssid.length()) WiFi.begin(gSet.ssid.c_str(), gSet.pass.c_str());
  }
}

std::string screenUrl()
{
  if (WiFi.status() == WL_CONNECTED) return std::string("http://") + WiFi.localIP().toString().c_str();
  if (s_ap) return std::string("http://") + WiFi.softAPIP().toString().c_str();
  return "";
}

#endif
