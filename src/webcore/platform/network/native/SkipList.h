/*
 * SkipList.h - resource blocklist driven by skipres.txt.
 *
 * One URL fragment per line, '#' starts a comment. A request whose URL contains
 * any listed fragment is dropped before it reaches the network. The file is read
 * from AGB_SKIPRES, then ./skipres.txt, then the directory of the executable.
 */

#ifndef SkipList_h
#define SkipList_h

#include <string>

namespace Net {

/* Reads (and caches) the skip list. Returns true when `url` matches an entry. */
bool isSkippedResource(const std::string& url);

/* Path of the file that was loaded (empty when none was found). */
const char* skipListPath();

/* Re-reads the file on the next query (tests use it after editing the list). */
void reloadSkipList();

} /* namespace Net */

#endif /* SkipList_h */
