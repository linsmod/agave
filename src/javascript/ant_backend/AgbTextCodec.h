#ifndef AgbTextCodec_h
#define AgbTextCodec_h

namespace Agb {

/* Routes ant's TextDecoder legacy labels (gbk, big5, shift_jis, ...) through
 * WebCore's own codec tables. Call once before the first engine is created;
 * ant keeps the pointer for the life of the process. */
void installTextCodecProvider();

} // namespace Agb

#endif
