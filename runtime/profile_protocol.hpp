// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <cstddef>
#include <cstdio>
namespace vrProfile {
struct Address {sockaddr_un value{};socklen_t size{};};
inline Address address(uid_t uid,dev_t device){
    Address a;a.value.sun_family=AF_UNIX;
    const int length=std::snprintf(a.value.sun_path+1,sizeof(a.value.sun_path)-1,
        "muxless-vr-profile-v1:%u:%u:%u",unsigned(uid),major(device),minor(device));
    a.size=static_cast<socklen_t>(offsetof(sockaddr_un,sun_path)+1+length);return a;
}
}
