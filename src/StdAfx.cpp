// StdAfx.cpp - pulls in 1C header implementation (once per DLL)
// 1C import libs are located by pragma comment(lib, HEADER1C_PATH "/libs/x.lib"),
// relative to the link cwd (= project root, see build.ps1).
#define HEADER1C_PATH "../../SQLite/_1Common/1CHEADERS"
#include "StdAfx.h"
#include <1CHEADERS/1cheaders.inl>
#include <fix_basic.inl>
#include <nocasemap.inl>
#include <dev_serv.cpp>
