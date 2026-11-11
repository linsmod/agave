/*
** webengine.h: webengine implements.
**
** Copyright(C) 2009 Zhang Ji Peng (onecoolx@gmail.com)
**
*/

#ifndef _WEBENGINE_H_
#define _WEBENGINE_H_

namespace MaCross {
	class WebView;
}
using namespace MaCross;

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct _mc_view {
	WebView * view;
	void    * data;
	void    * jscontext;   /* per-view Ant VM (AntRuntime::createContext) */
	int       hostId;      /* host view/tab id (window.__agbInfo().view) */
	char      lastTitle[256]; /* last reported document title */
};

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif/*_WEBENGINE_H_*/
