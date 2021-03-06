//@file bltree_test.cpp

/*
*    Copyright (C) 2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

/*
 * This is a derivative work.  The original 'C' source
 * code was put in the public domain by Karl Malbrain
 * (malbrain@cal.berkeley.edu.  The original copyright
 * notice is:
 *
 *     This work, including the source code, documentation
 *     and related data, is placed into the public domain.
 *
 *     The orginal author is Karl Malbrain.
 *
 *     THIS SOFTWARE IS PROVIDED AS-IS WITHOUT WARRANTY
 *     OF ANY KIND, NOT EVEN THE IMPLIED WARRANTY OF
 *     MERCHANTABILITY. THE AUTHOR OF THIS SOFTWARE,
 *     ASSUMES _NO_ RESPONSIBILITY FOR ANY CONSEQUENCE
 *     RESULTING FROM THE USE, MODIFICATION, OR
 *     REDISTRIBUTION OF THIS SOFTWARE.
 *
 */

#ifndef STANDALONE
#include "mongo/base/status.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/bltree/common.h"
#include "mongo/db/storage/bltree/blterr.h"
#include "mongo/db/storage/bltree/bltkey.h"
#include "mongo/db/storage/bltree/bltree.h"
#include "mongo/db/storage/bltree/bltval.h"
#include "mongo/db/storage/bltree/bufmgr.h"
#include "mongo/db/storage/bltree/logger.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/unittest/unittest.h"
#else
#include "common.h"
#include "blterr.h"
#include "bltkey.h"
#include "bltree.h"
#include "bltval.h"
#include "bufmgr.h"
#include "logger.h"
#endif

#include <errno.h>
#include <fstream>
#include <iostream>
#include <pthread.h>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <vector>

using namespace std;

namespace mongo {

    class BLTreeTestDriver {
    public:
        double getCpuTime( int type ) {
            struct rusage used[1];
            struct timeval tv[1];
        
            switch (type) {
            case 0: {
                gettimeofday( tv, NULL );
                return (double)tv->tv_sec + (double)tv->tv_usec / 1000000;
            }
            case 1: {
                getrusage( RUSAGE_SELF, used );
                return (double)used->ru_utime.tv_sec + (double)used->ru_utime.tv_usec / 1000000;
            }
            case 2: {
                getrusage( RUSAGE_SELF, used );
                return (double)used->ru_stime.tv_sec + (double)used->ru_stime.tv_usec / 1000000;
            } }
        
            return 0;
        }

        void printRUsage() {
            struct rusage used[1];
            getrusage( RUSAGE_SELF, used );

            cout
                << "\nProcess resource usage:"
                << "\nmaximum resident set size = " << used->ru_maxrss
                << "\nintegral shared memory size = " << used->ru_ixrss
                << "\nintegral unshared data size = " << used->ru_idrss
                << "\nintegral unshared stack size = " << used->ru_isrss
                << "\npage reclaims (soft page faults) = " << used->ru_minflt
                << "\npage faults (hard page faults) = " << used->ru_majflt
                << "\nswaps = " << used->ru_nswap
                << "\nblock input operations = " << used->ru_inblock
                << "\nblock output operations = " << used->ru_oublock
                << "\nIPC messages sent = " << used->ru_msgsnd
                << "\nIPC messages received = " << used->ru_msgrcv
                << "\nsignals received = " << used->ru_nsignals
                << "\nvoluntary context switches = " << used->ru_nvcsw
                << "\ninvoluntary context switches = " << used->ru_nivcsw << endl;

        }

        typedef struct {
            char type;
            char idx;
            char *infile;
            BufMgr* mgr;
            const char* thread;
        } ThreadArg;

        //
        // thread callback
        //
        static void* indexOp( void* arg ) {
            ThreadArg* args = (ThreadArg *)arg;

            int cnt   = 0;
            int len   = 0;

            uid next;
            uid page_no = LEAF_page;   // start on first page of leaves
            unsigned char key[256];
            PageSet set[1];
            BLTKey* ptr;
            BLTVal* val;
            BufMgr* mgr = args->mgr;
            FILE* in;
            int ch;
        
            BLTree* bt = BLTree::create( mgr );
        
            switch(args->type | 0x20) {
            case 'a': {
                cout << "started latch mgr audit" << endl;
                cnt = bt->latchaudit();
                cout << "finished latch mgr audit, found " << cnt << " keys" << endl;
                break;
            }
            case 'p': {
                cout << "started pennysort for " << args->infile << endl;
                uint32_t count = 0;
                if ( (in = fopen( args->infile, "rb" )) ) {
                    while (ch = getc(in), ch != EOF) {
                        if (ch == '\n') {
                            count++;

                        #ifndef STANDALONE
                            Status s = bt->insertkey( key, 10, 0, key + 10, len - 10 );
                            if (!s.isOK()) {
                                cerr << "Error " << bt->err << " Line: " << count << endl;
                                exit( -1 );
                            }
                        #else
                            if (bt->insertkey( key, 10, 0, key + 10, len - 10 )) {
                                cerr << "Error " << bt->err << " Line: " << count << endl;
                                exit( -1 );
                            }
                        #endif

                            len = 0;
                        }
                        else if (len < 255) {
                            key[len++] = ch;
                        }
                    }
                }
                else {
                    cerr << "error opening " << args->infile << endl;
                }
        
                cout << "finished " << args->infile << " for " << count << " keys" << endl;
                break;
            }
            case 'w': {
                cout << "started indexing for" << args->infile << endl;
                ifstream in( args->infile, ios::in );
                if (!in.good()) {
                    cerr << "error opening '" << args->infile << "'" << endl;
                    break;
                }
                string line;
                while (!in.eof()) {
                    getline( in, line);
                    if (0==line.size()) continue;
                    size_t n = line.find( '\t' );
                    if (string::npos==n) {
                        cerr << "bad input line: " << line << endl;
                        continue;
                    }
                    string key = line.substr( 0, n );
                    string val = line.substr( n+1 );
                    size_t m = line.size() - (n+1);

                #ifndef STANDALONE
                    Status s = bt->insertkey( (uchar*)key.c_str(), n, 0, (uchar*)val.c_str(), m );
                    if (!s.isOK()) {
                        cerr << "Error on line: " << line << endl;
                    }
                #else
                    if (bt->insertkey( (uchar*)key.c_str(), n, 0, (uchar*)val.c_str(), m )) {
                        cerr << "Error on line: " << line << endl;
                    }
                #endif
                }
                break;
            }
            case 'd': {
                cout << "started deleting keys for " << args->infile << endl; 
                uint32_t count = 0;
                if ( (in = fopen( args->infile, "rb" )) ) {
                    while (ch = getc(in), ch != EOF) {
                        if( ch == '\n' ) {
                            count++;
        
                        #ifndef STANDALONE
                            Status s = bt->deletekey( key, len, 0 );
                            if (!s.isOK()) {
                                cerr << "Error " << bt->err << " Line: " << count << endl;
                                exit( -1 );
                            }
                        #else
                            if (bt->deletekey( key, len, 0 )) {
                                cerr << "Error " << bt->err << " Line: " << count << endl;
                                exit( -1 );
                            }
                        #endif

                            len = 0;
                        }
                        else if (len < 255) {
                            key[len++] = ch;
                        }
                    }
                }
                else {
                    cerr << "error opening " << args->infile << endl;
                }
        
                cout << "finished " << args->infile << " for " << count << " keys" << endl;
                break;
            }
            case 'f': {
                cout << "started finding keys for" << args->infile << endl;
                ifstream in( args->infile, ios::in );
                if (!in.good()) {
                    cerr << "error opening '" << args->infile << "'" << endl;
                    break;
                }
                string line;
                uint32_t nlines = 0;
                uint32_t found = 0;
                char valbuf[128];
                uint32_t vallen;
                while (!in.eof()) {
                    getline( in, line );
                    if (0==line.size()) continue;
                    size_t n = line.find( '\t' );
                    if (string::npos==n) {
                        cerr << "bad input line: " << line << endl;
                        continue;
                    }
                    ++nlines;
                    string key = line.substr( 0, n );
                    if ( (vallen = bt->findkey( (uchar*)key.c_str(), n, (uchar*)valbuf, 128 )) ) {
                        cout << key << " -> " << string( valbuf, vallen ) << endl;
                        found++;
                    }
                }
                cerr << "finished " << args->infile << " for " << nlines << " keys, found " << found << endl;
                break;
            }
            case 's': {
                cerr << "started scanning" << endl;
                do {
                    if ( (set->pool = bt->mgr->pinpool( page_no )) ) {
                        set->page = bt->mgr->page( set->pool, page_no );
                    }
                    else {
                        break;
                    }
                    set->latch = bt->mgr->pinlatch( page_no );
                    bt->mgr->lockpage( LockRead, set->latch );
                    next = BLTVal::getid( set->page->right );
                    cnt += set->page->act;
        
                    for (unsigned int slot = 0; slot++ < set->page->cnt; ) {
                        if (next || slot < set->page->cnt) {
                            if (!slotptr(set->page, slot)->dead) {
                                ptr = keyptr(set->page, slot);
                                fwrite( ptr->key, ptr->len, 1, stdout );
                                fputc( ' ', stdout );
                                fputc( '-', stdout );
                                fputc( '>', stdout );
                                fputc( ' ', stdout );
                                val = valptr( set->page, slot );
                                fwrite( val->value, val->len, 1, stdout );
                                fputc( '\n', stdout );
                            }
                        }
                    }
                    bt->mgr->unlockpage( LockRead, set->latch );
                    bt->mgr->unpinlatch( set->latch );
                    bt->mgr->unpinpool( set->pool );
                } while ( (page_no = next) );
        
                cnt--;    // remove stopper key
                cout << " Total keys read " << cnt << endl;
                break;
            }
            case 'c':
                //posix_fadvise( bt->mgr->idx, 0, 0, POSIX_FADV_SEQUENTIAL);
        
                cout << "started counting" << endl;
                next = bt->mgr->latchmgr->nlatchpage + LATCH_page;
                page_no = LEAF_page;
        
                while (page_no < BLTVal::getid( bt->mgr->latchmgr->alloc->right )) {
                    uid off = page_no << bt->mgr->page_bits;
                    pread( bt->mgr->idx, bt->frame, bt->mgr->page_size, off );
                    if (!bt->frame->free && !bt->frame->lvl) {
                        cnt += bt->frame->act;
                    }
                    if (page_no > LEAF_page) {
                        next = page_no + 1;
                    }
                    page_no = next;
                }
                
                cnt--;    // remove stopper key
                cout << " Total keys read " << cnt  << endl;
                break;
            }
        
            bt->close();
            return NULL;
        }

        typedef struct timeval timer;

        Status drive( const std::string& dbname,  // index file name
                      const std::vector<std::string>& cmdv,  // cmd list 
                      const std::vector<std::string>& srcv,  // source key file list
                      uint pageBits,     // (i.e.) 32KB per page
                      uint poolSize,     // (i.e.) 4096 segments -> 4GB
                      uint segBits )     // (i.e.) 32 pages per segment -> 1MB 
        {
            if (poolSize > 65536) {
                cout << "poolSize too large, defaulting to 65536" << endl;
                poolSize = 65536;
            }
            
            cout <<
                " dbname = " << dbname <<
                "\n pageBits = " << pageBits <<
                "\n poolSize = " << poolSize <<
                "\n segBits = "  << segBits << std::endl;
        
            if (cmdv.size() != srcv.size()) {

            #ifndef STANDALONE
                return Status( ErrorCodes::InternalError,
                                "Need one command per source key file (per thread)." );
            #else
                return BTERR_struct;
            #endif

            }
        
            for (uint i=0; i<srcv.size(); ++i) {
                cout << " : " << cmdv[i] << " -> " << srcv[i] << endl;
            } 

            if (cmdv.size() == 1 && cmdv[0]=="Clear") {
                if (dbname.size()) {
                    remove( dbname.c_str() );
                }

            #ifndef STANDALONE
                return Status::OK();
            #else
                return BTERR_ok;
            #endif

            }
        
            double start = getCpuTime(0);
            uint cnt = srcv.size();
        
            if (cnt > 100) {

            #ifndef STANDALONE
                return Status( ErrorCodes::InternalError, "Cmd count exceeds 100." );
            #else
                return BTERR_struct;
            #endif

            }
        
            pthread_t threads[ cnt ];
            ThreadArg args[ cnt ];
            const char* threadNames[ 100 ] = {
              "main", "01", "02", "03", "04", "05", "06", "07", "08", "09",
                "10", "11", "12", "13", "14", "15", "16", "17", "18", "19",
                "20", "21", "22", "23", "24", "25", "26", "27", "28", "29",
                "30", "31", "32", "33", "34", "35", "36", "37", "38", "39",
                "40", "41", "42", "43", "44", "45", "46", "47", "48", "49",
                "50", "51", "52", "53", "54", "55", "56", "57", "58", "59",
                "60", "61", "62", "63", "64", "65", "66", "67", "68", "69",
                "70", "71", "72", "73", "74", "75", "76", "77", "78", "79",
                "80", "81", "82", "83", "84", "85", "86", "87", "88", "89",
                "90", "91", "92", "93", "94", "95", "96", "97", "98", "99" };
        
            // initialize logger
            vector< pair< string, ostream* > > v;
            v.push_back( pair< string, ostream* >( threadNames[ 0 ], &std::cout ) );
            for (uint i=1; i<=cnt; ++i) {
                v.push_back( pair< string, ostream* >( threadNames[ i ], new ostringstream() ) );
            }
            Logger::init( v );

            // allocate buffer pool manager
            char* name = (char *)dbname.c_str();
            BufMgr* mgr = BufMgr::create( name,         // index file name
                                          BT_rw,        // file open mode
                                          pageBits,     // page size in bits
                                          poolSize,     // number of segments
                                          segBits,      // segment size in pages in bits
                                          poolSize );   // hashSize
        
            if (!mgr) {

            #ifndef STANDALONE
                return Status( ErrorCodes::InternalError, "Buffer pool create failed." );
            #else
                return BTERR_struct;
            #endif

            }

            // start threads
            std::cout << " Starting:" << std::endl;
            for (uint i = 0; i < cnt; ++i) {
                std::cout << "  thread " << i << std::endl;
                args[i].infile = (char *)srcv[i].c_str();
                args[i].type = cmdv[i][0];   // (i.e.) A/W/D/F/S/C
                args[i].mgr = mgr;
                args[i].idx = i;
                args[i].thread = threadNames[ i+1 ];
                int err = pthread_create( &threads[i], NULL, BLTreeTestDriver::indexOp, &args[i] );
                if (err) {

                #ifndef STANDALONE
                    return Status( ErrorCodes::InternalError, "Error creating thread" );
                #else
                    return BTERR_struct;
                #endif

                }
            }
        
            // wait for termination
            std::cout << " Waiting for thread terminations." << std::endl;
            for (uint idx = 0; idx < cnt; ++idx) {
                pthread_join( threads[idx], NULL );
            }
        
            for (uint idx = 0; idx < cnt; ++idx) {
                const char* thread = args[ idx ].thread;
                std::cout << "\n*** Thread '" << thread << "' log ***" << std::endl;
                ostream* os = Logger::getStream( thread );
                if (NULL!=os) {
                    ostringstream* oss = dynamic_cast<ostringstream*>( os );
                    if (NULL!=oss) {
                        std::cout << oss->str() << std::endl;
                    }
                }
            }
        
            float elapsed0 = getCpuTime(0) - start;
            float elapsed1 = getCpuTime(1);
            float elapsed2 = getCpuTime(2);

            std::cout <<
                  " real "  << (int)(elapsed0/60) << "m " <<
                    elapsed0 - (int)(elapsed0/60)*60 << 's' <<
                "\n user "  << (int)(elapsed1/60) << "m " <<
                    elapsed1 - (int)(elapsed1/60)*60 << 's' <<
                "\n sys  "  << (int)(elapsed2/60) << "m " <<
                    elapsed2 - (int)(elapsed2/60)*60 << 's' << std::endl;
        
            printRUsage();

            BufMgr::destroy( mgr );

        #ifndef STANDALONE
            return Status::OK();
        #else
            return BTERR_ok;
        #endif

        }
    };

#ifndef STANDALONE

    //
    //  TESTS
    //

    TEST( BLTree, BasicWriteTest ) {

        BLTreeTestDriver driver;
        std::vector<std::string> cmdv;
        std::vector<std::string> srcv;

        /*
        // clear db
        cmdv.push_back( "Clear" );
        srcv.push_back( "" );
        ASSERT_OK( driver.drive( "testdb", cmdv, srcv, 15, 4096, 5 ) );
        */

        // two insert threads
        cmdv.clear();
        srcv.clear();
        cmdv.push_back( "Write" );    //[0]
        cmdv.push_back( "Write" );    //[1]
        srcv.push_back( "/home/paulpedersen/dev/bltree/kv.0" );
        srcv.push_back( "/home/paulpedersen/dev/bltree/kv.1" );

        ASSERT_OK( driver.drive( "testdb", cmdv, srcv, 15, 4096, 5 ) );

        // five find threads, two insert threads
        /*
        cmdv.clear();
        srcv.clear();
        cmdv.push_back( "Find" );    //[0]
        cmdv.push_back( "Find" );    //[1]
        cmdv.push_back( "Find" );    //[2]
        cmdv.push_back( "Find" );    //[3]
        cmdv.push_back( "Find" );    //[4]
        cmdv.push_back( "Write" );    //[5]
        cmdv.push_back( "Write" );    //[6]
        srcv.push_back( "/home/paulpedersen/dev/bltree/keys.0" );
        srcv.push_back( "/home/paulpedersen/dev/bltree/keys.1" );
        srcv.push_back( "/home/paulpedersen/dev/bltree/keys.2" );
        srcv.push_back( "/home/paulpedersen/dev/bltree/keys.3" );
        srcv.push_back( "/home/paulpedersen/dev/bltree/keys.4" );
        srcv.push_back( "/home/paulpedersen/dev/bltree/keys.5" );
        srcv.push_back( "/home/paulpedersen/dev/bltree/keys.6" );

        ASSERT_OK( driver.drive( "testdb", cmdv, srcv, 15, 4096, 5 ) );
        */

    }

#endif
    
}   // namespace mongo


#ifdef STANDALONE

void usage( const char* arg0 ) {
    cout << "Usage: " << arg0  << "OPTIONS\n"
            "  -f dbname      - the name of the index file(s)\n"
            "  -c cmd         - one of: Audit, Write, Delete, Find, Scan, Count\n"
            "  -p PageBits    - page size in bits; default 16\n"
            "  -n PoolEntrySize    - number of buffer pool mmapped page segments; default 8192\n"
            "  -s SegBits     - segment size in pages in bits; default 5\n"
            "  -k k_1,k_2,..  - list of source key files k_i, one per thread" << endl;
}

int main(int argc, char* argv[] ) {

    string dbname;          // index file name
    string cmd;             // command = { Audit|Write|Delete|Find|Scan|Count }
    uint pageBits = 16;     // (i.e.) 32KB per page
    uint poolSize = 8192;   // (i.e.) 8192 segments
    uint segBits  = 5;      // (i.e.) 32 pages per segment

    mongo::BLTreeTestDriver driver;
    vector<string> srcv;    // source files containing keys
    vector<string> cmdv;    // corresponding commands

    opterr = 0;
    char c;
    while ((c = getopt( argc, argv, "f:c:p:n:s:k:" )) != -1) {
        switch (c) {
        case 'f': { // -f dbName
            dbname = optarg;
            break;
        }
        case 'c': { // -c (Read|Write|Scan|Delete|Find)[,..]
            //cmd = optarg;
            char sep[] = ",";
            char* tok = strtok( optarg, sep );
            while (tok) {
                if (strspn( tok, " \t" ) == strlen(tok)) continue;
                cmdv.push_back( tok );
                tok = strtok( 0, sep );
            }
            break;
        }
        case 'p': { // -p pageBits
            pageBits = strtoul( optarg, NULL, 10 );
            break;
        }
        case 'n': { // -n poolSize
            poolSize = strtoul( optarg, NULL, 10 );
            break;
        }
        case 's': { // -s segBits
            segBits = strtoul( optarg, NULL, 10 );
            break;
        }
        case 'k': { // -k keyFile1,keyFile2,..
            char sep[] = ",";
            char* tok = strtok( optarg, sep );
            while (tok) {
                if (strspn( tok, " \t" ) == strlen(tok)) continue;
                srcv.push_back( tok );
                tok = strtok( 0, sep );
            }
            break;
        }
        case '?': {
            usage( argv[0] );
            return 1;
        }
        default: exit(-1);
        }
    }

    if (driver.drive( "testdb", cmdv, srcv, pageBits, poolSize, segBits )) {
        cout << "driver returned error" << endl;
    }

}
#endif

