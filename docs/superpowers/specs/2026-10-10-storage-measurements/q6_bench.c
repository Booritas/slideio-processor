/* THROWAWAY SPIKE - measurement 2: annotation write rate (SDS 03 sections 3.7, 3.12).
   The second of UR-EXE-8's two ten-million-row write passes. Measurement 1
   settled the object pass; this one has never been measured at all.

   Runs against a COPY of the measurement 1 mode-2 store, so the annotation
   pass competes with a real 3 GB objects table for page cache, as it will in
   production. Object ids are 1..9928772, contiguous.

   Modes:
     0  labels, WITHOUT ROWID, PRIMARY KEY(object_id,stage_id,label_id)
          - clustered by object, which is what labelCount(ObjectId) reads
     1  labels, rowid PK + UNIQUE INDEX(object_id,stage_id,label_id)
          - appended table, random-ish index: the 3.12 question again
     2  measurements, 3 per object, name stored as TEXT on every row
     3  measurements, 3 per object, name interned to an id
          - 2 vs 3 is UR-RES-17's reasoning applied to measurement names,
            which 3.7 never did

   Order:
     0  ascending object id   - a stage sweeping tiles in the same order
     1  shuffled              - the pessimal bound, no locality at all
     2  tile-shuffled         - the realistic case: tiles out of sequence,
                                objects contiguous within a tile

   Idempotency is 3.7's, so every insert is INSERT OR IGNORE. */
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define STAGE_ID 7
#define NLABELS 10
#define NTILES  7742   /* 98 x 79 tiles of 1024 px over the reference slide */

static double now_s(void){
    LARGE_INTEGER f,t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
    return (double)t.QuadPart/(double)f.QuadPart;
}
static void exec_sql(sqlite3*db,const char*sql){
    char*err=0;
    if(sqlite3_exec(db,sql,0,0,&err)!=SQLITE_OK){
        fprintf(stderr,"FAIL exec [%s]: %s\n",sql,err); exit(1);
    }
}
static unsigned long long rs=88172645463325252ULL;
static void rng_reset(void){ rs=88172645463325252ULL; }
static unsigned long long xr(void){ rs^=rs<<13; rs^=rs>>7; rs^=rs<<17; return rs; }

/* Deliberately long and repetitive, as real measurement names are. The point
   of mode 2 against mode 3 is what these cost when stored on every row. */
static const char* MEAS_NAMES[3] = {
    "nucleus_area_um2",
    "dab_optical_density_mean",
    "haematoxylin_optical_density_mean"
};

static long long file_size(const char*p){
    HANDLE h=CreateFileA(p,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,0,OPEN_EXISTING,0,0);
    if(h==INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER s; GetFileSizeEx(h,&s); CloseHandle(h); return s.QuadPart;
}

static void schema(sqlite3*db,int mode){
    if(mode==0){
        exec_sql(db,"CREATE TABLE label_assertions("
            " object_id INTEGER NOT NULL, stage_id INTEGER NOT NULL,"
            " label_id INTEGER NOT NULL,"
            " PRIMARY KEY(object_id,stage_id,label_id)) WITHOUT ROWID;");
    } else if(mode==1){
        exec_sql(db,"CREATE TABLE label_assertions("
            " id INTEGER PRIMARY KEY, object_id INTEGER NOT NULL,"
            " stage_id INTEGER NOT NULL, label_id INTEGER NOT NULL);");
        exec_sql(db,"CREATE UNIQUE INDEX ux_la ON label_assertions"
            "(object_id,stage_id,label_id);");
    } else if(mode==2){
        exec_sql(db,"CREATE TABLE measurement_assertions("
            " object_id INTEGER NOT NULL, stage_id INTEGER NOT NULL,"
            " name TEXT NOT NULL, value REAL NOT NULL, unit INTEGER NOT NULL,"
            " PRIMARY KEY(object_id,stage_id,name)) WITHOUT ROWID;");
    } else {
        exec_sql(db,"CREATE TABLE measurement_names("
            " name_id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL);");
        exec_sql(db,"CREATE TABLE measurement_assertions("
            " object_id INTEGER NOT NULL, stage_id INTEGER NOT NULL,"
            " name_id INTEGER NOT NULL, value REAL NOT NULL, unit INTEGER NOT NULL,"
            " PRIMARY KEY(object_id,stage_id,name_id)) WITHOUT ROWID;");
        for(int i=0;i<3;i++){
            char b[256];
            sprintf(b,"INSERT INTO measurement_names VALUES(%d,'%s');",i+1,MEAS_NAMES[i]);
            exec_sql(db,b);
        }
    }
}

int main(int argc,char**argv){
    if(argc<4){ fprintf(stderr,"usage: q6_bench <db> <mode> <order>\n"); return 2; }
    const char* path=argv[1]; int mode=atoi(argv[2]); int order=atoi(argv[3]);
    const char* modename[4]={"labels-clustered","labels-rowid+index",
                             "measurements-text-name","measurements-interned-name"};
    long long base=file_size(path);

    sqlite3*db;
    if(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE,0)!=SQLITE_OK){
        fprintf(stderr,"FAIL open %s\n",path); return 1; }
    exec_sql(db,"PRAGMA journal_mode=WAL;");
    exec_sql(db,"PRAGMA synchronous=NORMAL;");
    exec_sql(db,"PRAGMA cache_size=-262144;");
    exec_sql(db,"PRAGMA temp_store=MEMORY;");
    exec_sql(db,"PRAGMA mmap_size=1073741824;");
    schema(db,mode);

    long long n=0; sqlite3_stmt*s;
    sqlite3_prepare_v2(db,"SELECT count(*) FROM objects",-1,&s,0);
    if(sqlite3_step(s)==SQLITE_ROW) n=sqlite3_column_int64(s,0);
    sqlite3_finalize(s);

    /* The visit order over object ids. Shuffled needs the whole permutation
       resident - 80 MB at this scale, outside the store's own budget. */
    long long* ids=(long long*)malloc((size_t)n*sizeof(long long));
    if(!ids){ fprintf(stderr,"FAIL alloc\n"); return 1; }
    for(long long i=0;i<n;i++) ids[i]=i+1;
    rng_reset();
    if(order==1){
        for(long long i=n-1;i>0;i--){
            long long j=(long long)(xr()%(unsigned long long)(i+1));
            long long t=ids[i]; ids[i]=ids[j]; ids[j]=t;
        }
    } else if(order==2){
        /* The realistic case, between the two bounds. Objects are contiguous
           in object_id within a tile because the producing stage emitted them
           tile by tile; workers finish tiles out of sequence. So shuffle the
           runs and keep each run ascending. 98 x 79 tiles of 1024 px over the
           reference slide is 7742 runs of about 1282 objects. */
        long long runlen = n / NTILES; if(runlen < 1) runlen = 1;
        long long nruns = (n + runlen - 1) / runlen;
        long long* start = (long long*)malloc((size_t)nruns*sizeof(long long));
        long long* out = (long long*)malloc((size_t)n*sizeof(long long));
        if(!start || !out){ fprintf(stderr,"FAIL alloc\n"); return 1; }
        for(long long r=0;r<nruns;r++) start[r]=r*runlen;
        for(long long r=nruns-1;r>0;r--){
            long long j=(long long)(xr()%(unsigned long long)(r+1));
            long long t=start[r]; start[r]=start[j]; start[j]=t;
        }
        long long w=0;
        for(long long r=0;r<nruns;r++){
            long long s=start[r], e=s+runlen; if(e>n) e=n;
            for(long long k=s;k<e;k++) out[w++]=k+1;
        }
        free(start); free(ids); ids=out;
        printf("  (tile order: %lld runs of %lld, shuffled)\n", nruns, runlen);
    }
    rng_reset();

    const char* isql;
    if(mode==0) isql="INSERT OR IGNORE INTO label_assertions VALUES(?,?,?)";
    else if(mode==1) isql="INSERT OR IGNORE INTO label_assertions(object_id,stage_id,label_id) VALUES(?,?,?)";
    else isql="INSERT OR IGNORE INTO measurement_assertions VALUES(?,?,?,?,?)";
    sqlite3_stmt*ins;
    if(sqlite3_prepare_v2(db,isql,-1,&ins,0)!=SQLITE_OK){
        fprintf(stderr,"FAIL prep: %s\n",sqlite3_errmsg(db)); return 1; }

    int per = (mode<2) ? 1 : 3;
    long long rows=0, batch=10000, inbatch=0, mark=1000000, written=0;
    printf("mode=%d (%s) order=%s objects=%lld rows=%lld\n",
           mode,modename[mode],
           order==0?"ascending":(order==1?"shuffled":"tile-shuffled"),n,n*per);
    fflush(stdout);

    double t0=now_s(), tmark=t0;
    exec_sql(db,"BEGIN");
    for(long long i=0;i<n;i++){
        long long oid=ids[i];
        for(int k=0;k<per;k++){
            if(mode<2){
                sqlite3_bind_int64(ins,1,oid);
                sqlite3_bind_int(ins,2,STAGE_ID);
                sqlite3_bind_int(ins,3,1+(int)(xr()%NLABELS));
            } else {
                sqlite3_bind_int64(ins,1,oid);
                sqlite3_bind_int(ins,2,STAGE_ID);
                if(mode==2) sqlite3_bind_text(ins,3,MEAS_NAMES[k],-1,SQLITE_STATIC);
                else        sqlite3_bind_int(ins,3,k+1);
                sqlite3_bind_double(ins,4,(double)(xr()%100000)/1000.0);
                sqlite3_bind_int(ins,5,2);
            }
            if(sqlite3_step(ins)!=SQLITE_DONE){
                fprintf(stderr,"FAIL insert: %s\n",sqlite3_errmsg(db)); return 1; }
            sqlite3_reset(ins);
            rows++;
        }
        if(++inbatch>=batch){
            exec_sql(db,"COMMIT"); exec_sql(db,"BEGIN"); inbatch=0;
        }
        if(i+1>=mark){
            double tn=now_s();
            printf("  %6.1fM obj  %8.0f rows/s (interval)  elapsed %7.1fs\n",
                   (double)(i+1)/1e6,(double)(rows-written)/(tn-tmark),tn-t0);
            fflush(stdout);
            tmark=tn; written=rows; mark+=1000000;
        }
    }
    exec_sql(db,"COMMIT");
    double t1=now_s();
    sqlite3_finalize(ins);

    /* The checkpoint is INSIDE the timed region and reported separately. The
       first run left it outside, and Windows write-back then absorbed an
       unequal share of two passes differing 2x in bytes written - which made
       the larger table look 3.6x faster than the smaller one. Insert time
       alone is not the cost of a write pass. */
    exec_sql(db,"PRAGMA wal_checkpoint(TRUNCATE);");
    double t2=now_s();

    printf("ANNOTATE mode=%d order=%d %lld rows: insert %.1fs + checkpoint %.1fs"
           " = %.1fs total, %.0f rows/s\n",
           mode,order,rows,t1-t0,t2-t1,t2-t0,(double)rows/(t2-t0));
    fflush(stdout);

    /* Effective label set / asserted measurement read, the query 3.7 exposes. */
    {
        const char* qsql = (mode<2)
          ? "SELECT label_id,stage_id FROM label_assertions WHERE object_id=? ORDER BY label_id"
          : ((mode==2)
             ? "SELECT name,value,unit FROM measurement_assertions WHERE object_id=? ORDER BY name"
             : "SELECT name_id,value,unit FROM measurement_assertions WHERE object_id=? ORDER BY name_id");
        sqlite3_stmt*q; sqlite3_prepare_v2(db,qsql,-1,&q,0);
        int reps=20000; long long got=0; double r0=now_s();
        for(int i=0;i<reps;i++){
            sqlite3_bind_int64(q,1,(long long)(xr()%(unsigned long long)n)+1);
            while(sqlite3_step(q)==SQLITE_ROW) got++;
            sqlite3_reset(q);
        }
        double r1=now_s();
        printf("  read annotations of one object  %7.4f ms  (%lld rows over %d objects)\n",
               (r1-r0)*1000.0/reps,got,reps);
        sqlite3_finalize(q);
    }

    sqlite3_close(db);
    long long after=file_size(path);
    printf("  store %lld -> %lld bytes, +%lld (%.1f bytes/row)\n",
           base,after,after-base,(double)(after-base)/(double)rows);
    free(ids);
    return 0;
}
