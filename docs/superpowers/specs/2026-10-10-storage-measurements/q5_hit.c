/* THROWAWAY - measurement 1 addendum: get(ObjectId) on the HIT path.
   The q5_bench point lookup probed synthetic anchors and hit 31/20000, so it
   timed the miss path, where mode 2 never takes the extra table hop. Here the
   keys are sampled from the store first, so every lookup resolves. Sampling
   warms the mode 0/1 lookup path more than mode 2's, which biases in mode 0's
   favour - deliberate, since the question is whether mode 2's hop costs. */
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#define REPS 20000
static double now_s(void){
    LARGE_INTEGER f,t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
    return (double)t.QuadPart/(double)f.QuadPart;
}
static unsigned long long rs = 88172645463325252ULL;
static unsigned long long xr(void){ rs^=rs<<13; rs^=rs>>7; rs^=rs<<17; return rs; }
int main(int argc,char**argv){
    if(argc<3){ fprintf(stderr,"usage: q5_hit <db> <mode>\n"); return 2; }
    int mode=atoi(argv[2]); sqlite3*db;
    if(sqlite3_open_v2(argv[1],&db,SQLITE_OPEN_READONLY,0)!=SQLITE_OK){
        fprintf(stderr,"open fail\n"); return 1; }
    sqlite3_exec(db,"PRAGMA cache_size=-262144;",0,0,0);
    sqlite3_exec(db,"PRAGMA mmap_size=1073741824;",0,0,0);

    long long maxid=0; sqlite3_stmt*s;
    sqlite3_prepare_v2(db,"SELECT max(id) FROM objects",-1,&s,0);
    if(sqlite3_step(s)==SQLITE_ROW) maxid=sqlite3_column_int64(s,0);
    sqlite3_finalize(s);

    static long long keys[REPS]; int n=0;
    if(mode==2){
        sqlite3_prepare_v2(db,"SELECT anchor FROM objects WHERE id=?",-1,&s,0);
        while(n<REPS){
            sqlite3_bind_int64(s,1,(long long)(xr()%(unsigned long long)maxid)+1);
            if(sqlite3_step(s)==SQLITE_ROW) keys[n++]=sqlite3_column_int64(s,0);
            sqlite3_reset(s);
        }
    } else {
        sqlite3_prepare_v2(db,"SELECT id FROM objects WHERE id>=? LIMIT 1",-1,&s,0);
        while(n<REPS){
            sqlite3_bind_int64(s,1,(long long)(xr()%(unsigned long long)maxid));
            if(sqlite3_step(s)==SQLITE_ROW) keys[n++]=sqlite3_column_int64(s,0);
            sqlite3_reset(s);
        }
    }
    sqlite3_finalize(s);

    sqlite3_stmt*q; long long hits=0;
    sqlite3_prepare_v2(db, mode==2
        ? "SELECT id,geom FROM objects WHERE layer=3 AND anchor=?"
        : "SELECT id,geom FROM objects WHERE id=?", -1,&q,0);
    double t0=now_s();
    for(int i=0;i<REPS;i++){
        sqlite3_bind_int64(q,1,keys[i]);
        if(sqlite3_step(q)==SQLITE_ROW) hits++;
        sqlite3_reset(q);
    }
    double t1=now_s();
    printf("mode=%d  get(ObjectId) HIT path  %7.4f ms/lookup  (%lld hits of %d)\n",
           mode,(t1-t0)*1000.0/REPS,hits,REPS);
    sqlite3_finalize(q); sqlite3_close(db); return 0;
}
