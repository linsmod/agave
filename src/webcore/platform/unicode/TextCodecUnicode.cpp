
#include <stdlib.h>
#include <string.h>

#include "charset.h"
#include "TextCodecUnicode.h"

class UTextCodec
{
public:
	UTextCodec(const char* name)
		: m_ops(create_charset_ops(name))
	{
	}

	~UTextCodec()
	{
		destory_charset_ops(m_ops);
	}

	const char* charset() { return m_ops ? m_ops->charset : 0;}

	charset_ops * ops() { return m_ops; }
private:
    charset_ops * m_ops;
};

UTextCodec * CreateCodec(const char* name)
{
	return new UTextCodec(name);
}

void DestoryCodec(UTextCodec* codec)
{
	delete codec;
}

const char * GetCodecName(UTextCodec* codec)
{
	return codec->charset();
}

/* Length in bytes of the character at `str`, or <= 0 when it is incomplete. */
int UTextCharLen(UTextCodec* codec, const unsigned char* str, int str_len)
{
	if (!codec || !codec->ops() || !str || str_len <= 0)
		return -1;

	return codec->ops()->len_first_char(str, str_len);
}

int UTextDecode(UTextCodec* codec, UChar16* dest, int n, const unsigned char* mstr, int mstr_len, int* conved_mstr_len)
{
	if (!codec || !codec->ops() || !dest || n <=0 || !mstr || mstr_len<=0)
		return -1;

    int count = 0;
	int left = mstr_len;

	while (left > 0) {
		int len_cur_char = codec->ops()->len_first_char(mstr, left);
		if (len_cur_char <= 0){ //first char can not be decode
			if ((*mstr) && (*mstr <= 0x7F)) { //is ascii char except NULL
				dest[count] = *((const unsigned char*)mstr);
				count++;
			}
			mstr++; left--;
			continue;
		}

		/* Four-byte UTF-8 sequences decode to a code point above 0xFFFF, which
		 * does not fit in one UChar16: conv_to_uchar truncates them, producing a
		 * lone low surrogate. Rebuild the code point here and emit a surrogate
		 * pair, otherwise strings (and scripts) containing emoji get corrupted. */
		if (len_cur_char == 4 && GetCodecName(codec) && !strcmp(GetCodecName(codec), "UTF-8")) {
			unsigned long cp = ((unsigned long)(mstr[0] & 0x07) << 18)
			                 | ((unsigned long)(mstr[1] & 0x3F) << 12)
			                 | ((unsigned long)(mstr[2] & 0x3F) << 6)
			                 | ((unsigned long)(mstr[3] & 0x3F));

			if (cp >= 0x10000 && cp <= 0x10FFFF) {
				unsigned long v = cp - 0x10000;

				dest[count++] = (UChar16)(0xD800 + (v >> 10));
				if (count == n)
					break;
				dest[count++] = (UChar16)(0xDC00 + (v & 0x3FF));
				if (count == n)
					break;

				mstr += len_cur_char;
				left -= len_cur_char;
				continue;
			}
		}

		{
		UChar16 uc = codec->ops()->conv_to_uchar(mstr, len_cur_char);
		dest[count] = uc;
		}

		count++;

		if (count == n)
			break;

		mstr += len_cur_char;
		left -= len_cur_char;
	}

	if (dest && count < n) {
		dest[count] = 0;
	}

    if (conved_mstr_len)
        *conved_mstr_len = mstr_len - left;
    return count;
}

int UTextEncode(UTextCodec* codec, unsigned char* dest, int n, const UChar16 *wcs, int wcs_len, int* conved_wcs_len)
{
	if (!codec || !codec->ops() || !dest || n <=0 || !wcs || wcs_len<=0)
		return -1;

    int wc_count = 0, count = 0;
    unsigned char mchar [16] = {0};
	UChar16 ucode = 0;

	while (wc_count < wcs_len) {
		ucode = *wcs;

		/* UTF-8: combine a surrogate pair into one code point and emit four
		 * bytes. Encoding each half separately produces CESU-8 (ED A0 xx), which
		 * is not what UTF-8 consumers expect and corrupts strings. */
		if (GetCodecName(codec) && !strcmp(GetCodecName(codec), "UTF-8")
		    && ucode >= 0xD800 && ucode <= 0xDBFF && wc_count + 1 < wcs_len) {
			UChar16 low = wcs[1];

			if (low >= 0xDC00 && low <= 0xDFFF) {
				unsigned long cp = 0x10000
				                 + (((unsigned long)(ucode - 0xD800)) << 10)
				                 + (unsigned long)(low - 0xDC00);
				unsigned char four[4];

				four[0] = (unsigned char)(0xF0 | (cp >> 18));
				four[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
				four[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
				four[3] = (unsigned char)(0x80 | (cp & 0x3F));

				if (count + 4 > n)
					break;

				memcpy(dest, four, 4);
				dest += 4;
				count += 4;
				wc_count += 2;
				wcs += 2;
				continue;
			}
		}
        int len_cur_char = codec->ops()->conv_from_uchar(ucode, mchar);
		if (!len_cur_char) {
			*dest = (unsigned char)ucode;
			wcs++; wc_count++; dest++; count++;
			continue;
		}

		if (count + len_cur_char > n)
			break;
		
		if (dest) {
			memcpy(dest, mchar, len_cur_char);
			dest += len_cur_char;
		}
		
		count += len_cur_char;
		wc_count++;
		wcs++;
	}

	if (dest && count < n)
		*dest = '\0';

    if (conved_wcs_len)
        *conved_wcs_len = wc_count;

    return count;
}

