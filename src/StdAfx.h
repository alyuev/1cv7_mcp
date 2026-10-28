// StdAfx.h - minimal, mirrors SQL_DBF\StdAfx.h
#pragma once
#define _MSC_VER 1200
#define VC_EXTRALEAN

#include <afxwin.h>
#include <afxext.h>
#ifndef _AFX_NO_OLE_SUPPORT
#include <afxole.h>
#include <afxodlgs.h>
#include <afxdisp.h>
#endif
#ifndef _AFX_NO_DB_SUPPORT
#include <afxdb.h>
#endif
#define _AFX_NO_DAO_SUPPORT
#define _SECURE_SCL 0
#include <afxdtctl.h>
#ifndef _AFX_NO_AFXCMN_SUPPORT
#include <afxcmn.h>
#endif

#include <1CHEADERS/1cheaders.h>
#include <traps.hpp>
#include <nocasemap.hpp>
#include <dev_serv.h>
