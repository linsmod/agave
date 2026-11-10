/*
 * RealmTags.h - native pointer tags shared by the realm/document bindings.
 *
 * The tags live here instead of AntRuntime.cpp so new modules (RealmDocument)
 * can look up the document a JS wrapper belongs to without depending on
 * AntRuntime's internals.
 */

#ifndef RealmTags_h
#define RealmTags_h

#define AGB_TAG_NODE     0x4E4F4431u   /* 'NOD1'  Node* / Document*          */
#define AGB_TAG_WINDOW   0x574E4431u   /* 'WND1'  Document* (window proxy)   */
#define AGB_TAG_LOCATION 0x4C4F4331u   /* 'LOC1'  Document* (Location)       */
#define AGB_TAG_HISTORY  0x48495331u   /* 'HIS1'  FrameHistory* (History)    */
#define AGB_TAG_COLLECTION 0x434F4C31u /* 'COL1'  AgbCollection* (NodeList/HTMLCollection wrapper) */
#define AGB_TAG_WRAPREALM 0x57525031u  /* 'WRP1'  AntbRealm* the wrapper was created in        */

#endif /* RealmTags_h */
