#ifndef C120_MP4_H
#define C120_MP4_H
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Top-level boxes only, with constant memory. Keep the last whole moof/mdat pair. */
static off_t mp4_complete_bytes(int fd,int strict)
{
    struct stat st; if (fstat(fd,&st)) return 0;
    off_t at=0,last=0; int moov=0,moof=0,fragmented=0,mdat=0;
    while (at<=st.st_size-8) {
        unsigned char h[16]; if (pread(fd,h,8,at)!=8) break;
        uint64_t size=((uint64_t)h[0]<<24)|((uint64_t)h[1]<<16)|((uint64_t)h[2]<<8)|h[3];
        unsigned header=8;
        if (size==1) { if (pread(fd,h+8,8,at+8)!=8) break; size=0; for (int i=8;i<16;++i) size=(size<<8)|h[i]; header=16; }
        if (size<header || size>(uint64_t)(st.st_size-at)) break;
        if (!memcmp(h+4,"moov",4)) moov=1;
        if (!memcmp(h+4,"moof",4)) moof=fragmented=1;
        if (!memcmp(h+4,"mdat",4)) mdat=1;
        at+=(off_t)size;
        if (!memcmp(h+4,"mdat",4) && moov && moof) { last=at; moof=0; }
    }
    if (!fragmented && moov && mdat && at==st.st_size) last=at;
    return strict && (at!=st.st_size || moof)?0:last;
}
#endif
