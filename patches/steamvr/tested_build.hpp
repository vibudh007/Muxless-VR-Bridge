#pragma once
#include <cstring>
#define TESTED_COMPOSITOR_SHA256 "7695e718ee0d070df596663d737b6a183aa650a2c66b90a035cc7f227a7a49e1"
#define TESTED_COMPOSITOR_2193_SHA256 "b3747b541c86f683286f13fcce9ed7761e0258ecef3827c5ce915167226947fe"
struct BuildPolicy {
 bool descriptors=false, transferRing=false, shader=false, observer=false;
 bool any() const { return descriptors || transferRing || shader; }
};
inline BuildPolicy policyFor(const char*sha256) {
 if(!sha256)return {};
 if(!std::strcmp(sha256,TESTED_COMPOSITOR_SHA256))return {true,true,true,true};
 // SVR-001 and SVR-006 are verified against this executable. In particular,
 // the descriptor observer's native instruction offsets belong to 2.19.2.
 if(!std::strcmp(sha256,TESTED_COMPOSITOR_2193_SHA256))return {true,true,false,false};
 return {};
}
