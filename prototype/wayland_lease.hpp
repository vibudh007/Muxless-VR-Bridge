#pragma once
#include <wayland-client.h>
#include "drm-lease-client.h"
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <string>
#include <memory>
#include <vector>
#include <iostream>
#include <stdexcept>
struct LeaseDevice;
struct Connector { wp_drm_lease_connector_v1 *obj=nullptr; LeaseDevice *owner=nullptr; uint32_t id=0; std::string name,description; bool withdrawn=false; };
struct LeaseDevice { wp_drm_lease_device_v1 *obj=nullptr; int fd=-1; std::vector<std::unique_ptr<Connector>> connectors; };
struct Wayland {
 wl_display *display=nullptr; wl_registry *registry=nullptr; wp_drm_lease_v1 *lease=nullptr;
 int leaseFd=-1; bool finished=false; std::vector<std::unique_ptr<LeaseDevice>> devices;
 static void cname(void *p,wp_drm_lease_connector_v1*,const char*s){static_cast<Connector*>(p)->name=s;}
 static void cdesc(void *p,wp_drm_lease_connector_v1*,const char*s){static_cast<Connector*>(p)->description=s;}
 static void cid(void *p,wp_drm_lease_connector_v1*,uint32_t n){static_cast<Connector*>(p)->id=n;}
 static void cdone(void*,wp_drm_lease_connector_v1*){}
 static void cwithdraw(void *p,wp_drm_lease_connector_v1*){static_cast<Connector*>(p)->withdrawn=true;}
 static void dfd(void *p,wp_drm_lease_device_v1*,int32_t fd){static_cast<LeaseDevice*>(p)->fd=fd;}
 static void connector(void *p,wp_drm_lease_device_v1*,wp_drm_lease_connector_v1 *obj){
  auto*d=static_cast<LeaseDevice*>(p);auto c=std::make_unique<Connector>();c->obj=obj;c->owner=d;
  static const wp_drm_lease_connector_v1_listener l={cname,cdesc,cid,cdone,cwithdraw};
  wp_drm_lease_connector_v1_add_listener(obj,&l,c.get());d->connectors.push_back(std::move(c));
 }
 static void ddone(void*,wp_drm_lease_device_v1*){} static void dreleased(void*,wp_drm_lease_device_v1*){}
 static void global(void*p,wl_registry*r,uint32_t name,const char*interface,uint32_t){
  if(std::strcmp(interface,"wp_drm_lease_device_v1"))return;
  auto*w=static_cast<Wayland*>(p);auto d=std::make_unique<LeaseDevice>();
  d->obj=static_cast<wp_drm_lease_device_v1*>(wl_registry_bind(r,name,&wp_drm_lease_device_v1_interface,1));
  static const wp_drm_lease_device_v1_listener l={dfd,connector,ddone,dreleased};
  wp_drm_lease_device_v1_add_listener(d->obj,&l,d.get());w->devices.push_back(std::move(d));
 }
 static void removed(void*,wl_registry*,uint32_t){}
 static void lfd(void*p,wp_drm_lease_v1*,int32_t fd){static_cast<Wayland*>(p)->leaseFd=fd;}
 static void lfinished(void*p,wp_drm_lease_v1*){static_cast<Wayland*>(p)->finished=true;}
 Wayland(){
  display=wl_display_connect(nullptr);if(!display)throw std::runtime_error("Cannot connect to Wayland");
  registry=wl_display_get_registry(display);static const wl_registry_listener l={global,removed};wl_registry_add_listener(registry,&l,this);
  for(int i=0;i<3;i++)if(wl_display_roundtrip(display)<0)throw std::runtime_error("Wayland discovery failed");
 }
 Connector *select(const std::string &name){
  Connector *found=nullptr;
  for(auto&d:devices)for(auto&c:d->connectors)if(!c->withdrawn){
   struct stat st{};fstat(d->fd,&st);
   std::cout<<"OUTPUT "<<c->name<<" id="<<c->id<<" description="<<c->description<<" device="<<major(st.st_rdev)<<":"<<minor(st.st_rdev)<<"\n";
   if(name.empty()||name==c->name){if(found)throw std::runtime_error("Multiple outputs: specify --connector NAME");found=c.get();}
  }
  if(!found)throw std::runtime_error("No matching leasable headset output; connect headset and close SteamVR");
  return found;
 }
 void acquire(Connector*c){
  auto*request=wp_drm_lease_device_v1_create_lease_request(c->owner->obj);wp_drm_lease_request_v1_request_connector(request,c->obj);
  lease=wp_drm_lease_request_v1_submit(request);static const wp_drm_lease_v1_listener l={lfd,lfinished};wp_drm_lease_v1_add_listener(lease,&l,this);
  wl_display_flush(display);
  auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(leaseFd<0&&!finished&&std::chrono::steady_clock::now()<deadline){
   if(wl_display_dispatch_pending(display)<0)break;
   if(leaseFd>=0||finished)break;
   pollfd fd{wl_display_get_fd(display),POLLIN,0};if(poll(&fd,1,250)>0&&wl_display_dispatch(display)<0)break;
  }
  if(leaseFd<0)throw std::runtime_error(finished?"COSMIC denied the display lease":"Display lease timed out");
  std::cout<<"LEASE granted fd="<<leaseFd<<"\n";
 }
 ~Wayland(){
  if(lease)wp_drm_lease_v1_destroy(lease);
  if(leaseFd>=0)close(leaseFd);
  for(auto&d:devices){for(auto&c:d->connectors)wp_drm_lease_connector_v1_destroy(c->obj);if(d->fd>=0)close(d->fd);if(d->obj)wl_proxy_destroy(reinterpret_cast<wl_proxy*>(d->obj));}
  if(registry)wl_registry_destroy(registry);
  if(display){wl_display_flush(display);wl_display_disconnect(display);}
 }
};
