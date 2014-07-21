//@file buffer_mgr.cpp
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

#include "common.h"
#include "bltkey.h"
#include "buffer_mgr.h"
#include "latchMgr.h"
#include "logger.h"
#include "logger.h"
#include "page.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <memory.h>
#include <sstream>
#include <sys/mman.h>

#define BUFFER_MGR_TRACE    false

namespace mongo {

    BufferMgr* BufferMgr::create( const char* name,
                                  uint mode,
                                  uint bits0,
                                  uint poolMax,
                                  uint segSize,
                                  uint hashSize ) {
    
        if (BUFFER_MGR_TRACE) Logger::logDebug( "main", "", __LOC__ );

        assert( NULL != name );

        // determine sanity of page size and buffer pool
        uint bits = bits0;
        if (bits > BLT_maxbits) {
            __OSS__( "defaulting to BLT_maxbits = " << BLT_maxbits );
            Logger::logInfo( "main", __ss__, __LOC__ );
            bits = BLT_maxbits;
        }
        else if( bits < BLT_minbits ) {
            __OSS__( "defaulting to BLT_minbits = " << BLT_minbits );
            Logger::logInfo( "main", __ss__, __LOC__ );
            bits = BLT_minbits;
        }
    
        if (!poolMax) {
            Logger::logError( "main", "must specify buffer pool size.  bailing out.", __LOC__ );
            return NULL;    // must have buffer pool
        }
    
        BufferMgr* mgr = (BufferMgr*)calloc( 1, sizeof(BufferMgr) );
        int fd = open( name, O_RDWR | O_CREAT, 0666 );
        if (-1 == fd) {
            __OSS__( "open( " << name << " ) syserr: " << strerror(errno) );
            Logger::logError( "main", __ss__, __LOC__ );
            free(mgr);
            return NULL;
        }

        mgr->_fd = fd;

        uint cacheBlockSize = 4096;    // minimum mmap segment size for unix
        LatchMgr* latchMgr = (LatchMgr*)malloc( BLT_maxpage );

        uint amt[1];
        *amt = 0;
    
        // read minimum page size to get root info
        off_t fileSize = lseek( fd, 0L, SEEK_END );
        if (fileSize) {
            if (BLT_minpage == pread( fd, latchMgr, BLT_minpage, 0 )) {
                bits = latchMgr->_alloc->_bits;
            }
            else {
                __OSS__( "pread( " << name << " ) syserr: " << strerror(errno) );
                Logger::logError( "main", __ss__, __LOC__ );
                free(mgr);
                free(latchMgr);
                return NULL;
            }
        } else if (BLT_ro ==  mode) {
            Logger::logError( "main", "read only mode not currently supported.", __LOC__ );
            free(latchMgr);
            mgr->close( "main" );
            return NULL;
        }
    
        uint pageSize = (1 << bits);
        mgr->_pageSize = pageSize;
        mgr->_pageBits = bits;
        mgr->_poolMax  = poolMax;
        mgr->_mode = mode;

        if (cacheBlockSize < pageSize) {
            cacheBlockSize = pageSize;
        }
    
        // mask for partial memmaps
        mgr->_poolMask = (cacheBlockSize >> bits) - 1;
    
        // see if requested size of pages per memmap is greater
        if ((1 << segSize) > mgr->_poolMask) {
            mgr->_poolMask = (1 << segSize) - 1;
        }
    
        mgr->_segBits = 0;
        while ( (1 << mgr->_segBits) <= mgr->_poolMask) { mgr->_segBits++; }
    
        mgr->_hashSize = hashSize;
        mgr->_pool  = (Pool*)calloc( poolMax, sizeof(Pool) );
        mgr->_hash  = (ushort*)calloc( hashSize, sizeof(ushort) );
        mgr->_latch = (SpinLatch*)calloc( hashSize, sizeof(SpinLatch) );
        mgr->_zero  = (Page*)malloc( mgr->_pageSize );
        memset( mgr->_zero, 0, mgr->_pageSize );
    
        if (fileSize || *amt) {
            if (!mgr->mapLatches( "main" )) return NULL;
            free( latchMgr );
            return mgr;
        }
    
        // initialize an empty b-tree with
        //   - latch page,
        //   - root page,
        //   - page of leaves, and
        //   - page(s) of latches

        memset( latchMgr, 0, pageSize );
        uint nlatchPage = BLT_latchtable / (pageSize / sizeof(LatchSet)) + 1; 
        Page::putid( latchMgr->_alloc->_right, MIN_level + 1 + nlatchPage);
        latchMgr->_alloc->_bits = bits;
        latchMgr->_nlatchPage = nlatchPage;
        latchMgr->_latchTotal = nlatchPage * (pageSize / sizeof(LatchSet));
    
        // initialize latch manager
        uint latchHash = (pageSize - sizeof(LatchMgr)) / sizeof(HashEntry);
    
        // size of hash table = total number of latchsets
        if (latchHash > latchMgr->_latchTotal) {
            latchHash = latchMgr->_latchTotal;
        }
    
        latchMgr->_latchHash = latchHash;
    
        if (write( fd, latchMgr, pageSize) < pageSize) {
            __OSS__( "write( " << name << " ) syserr: " << strerror(errno) );
            Logger::logError( "main", __ss__, __LOC__ );
            mgr->close( "main" );
            return NULL;
        }
    
        memset( latchMgr, 0, pageSize );
        latchMgr->_alloc->_bits = bits;
    
        for (uint level = MIN_level; level--; ) {
            Page::slotptr(latchMgr->_alloc, 1)->_off = pageSize - 3;
            Page::putid( Page::slotptr(latchMgr->_alloc, 1)->_id,
                            level ? MIN_level - level + 1 : 0); // next lower page number
            BLTKey* key = Page::keyptr(latchMgr->_alloc, 1);
            key->_len = 2;        // create stopper key
            key->_key[0] = 0xff;
            key->_key[1] = 0xff;
            latchMgr->_alloc->_min = pageSize - 3;
            latchMgr->_alloc->_level = level;
            latchMgr->_alloc->_cnt = 1;
            latchMgr->_alloc->_act = 1;
            if (write(fd, latchMgr, pageSize) < pageSize) {
                __OSS__( "write( " << name << " ) syserr: " << strerror(errno) );
                Logger::logError( "main", __ss__, __LOC__ );
                mgr->close( "main" );
                return NULL;
            }
        }
    
        // clear out latch manager locks
        //   and rest of pages to round out segment
        memset( latchMgr, 0, pageSize);
        uint last = MIN_level + 1;
    
        while (last <= ((MIN_level + 1 + nlatchPage) | mgr->_poolMask)) {
            pwrite( fd, latchMgr, pageSize, (last << bits) );
            ++last;
        }

        if (!mgr->mapLatches( "main" )) return NULL;
        free( latchMgr );
        return mgr;
    }

    #define MAPLATCHES_TRACE    false

    bool BufferMgr::mapLatches( const char* thread ) {
        if (BUFFER_MGR_TRACE) Logger::logDebug( thread, "", __LOC__ );
 
        int prot = PROT_READ | PROT_WRITE;
        uint pageSize = _pageSize;
        int fd = _fd;
        off_t latchMgrOffset = ALLOC_page * pageSize;

        _latchMgr = (LatchMgr *)mmap( 0, pageSize, prot, MAP_SHARED, fd, latchMgrOffset );
        if (_latchMgr == MAP_FAILED) {
            __OSS__( "mmap failed on 'alloc' page, syserr: " << strerror(errno) );
            Logger::logError( thread, __ss__, __LOC__ );
            close( thread );
            return false;
        }

        uint latchSetsSize  = _latchMgr->_nlatchPage * pageSize;
        off_t latchSetsOffset = LATCH_page * pageSize;
        _latchMgr->_latchSets = (LatchSet *)mmap( 0, latchSetsSize, prot, MAP_SHARED, fd, latchSetsOffset );

        if (MAP_FAILED == _latchMgr->_latchSets) {
            __OSS__( "mmap failed on 'latch' page, syserr: " << strerror(errno) );
            Logger::logError( thread, __ss__, __LOC__ );
            close( thread );
            return false;
        }

        return true;
    }

    void BufferMgr::close( const char* thread ) {
        if (BUFFER_MGR_TRACE) Logger::logDebug( thread, "", __LOC__ );

        // release mapped pages. note: slot zero is never used
        for (uint slot = 1; slot < _poolMax; ++slot) {
            Pool* pool = &_pool[ slot ];
            if (pool->_slot) {
                munmap( pool->_map, (_poolMask+1) << _pageBits );
            }
        }
    
        munmap( _latchMgr->_latchSets, _latchMgr->_nlatchPage * _pageSize );
        munmap( _latchMgr, _pageSize );

        ::close( _fd );
        free( _pool );
        free( _hash );
        free( (void *)_latch );
    }
    

    /**
    *  find segment in pool
    *  must be called with hashslot at hashIndex locked
    *  @return NULL if not there, otherwise return node
    */
    Pool* BufferMgr::findPool( PageId pageId, uint hashIndex, const char* thread ) {
        if (BUFFER_MGR_TRACE) Logger::logDebug( thread, "", __LOC__ );

        // compute start of hash chain in pool
        Pool* pool;
        uint slot = _hash[ hashIndex ];
        if (slot) {
            pool = &_pool[ slot ];
        }
        else {
            return NULL;
        }
        pageId &= ~(_poolMask);

        while (pool->_basePage != pageId) {
            if ( (pool = (Pool *)pool->_hashNext) ) continue;
            return NULL;
        }
        return pool;
    }

	void BufferMgr::linkHash( Pool* pool, PageId pageId, int hashIndex, const char* thread ) {
        if (BUFFER_MGR_TRACE) Logger::logDebug( thread, "", __LOC__ );

        assert( NULL != pool );

	    pool->_hashPrev = pool->_hashNext = NULL;
	    pool->_basePage = pageId & ~(_poolMask);
	    pool->_pin = CLOCK_bit + 1;
	
	    uint slot = _hash[ hashIndex ];
	    if (slot) {
	        Pool* node = &_pool[ slot ];
	        pool->_hashNext = node;
	        node->_hashPrev = pool;
	    }
	    _hash[ hashIndex ] = pool->_slot;
	}
	
	BLTERR BufferMgr::mapSegment( Pool* pool, PageId pageId, const char* thread ) {
        if (BUFFER_MGR_TRACE) Logger::logDebug( thread, "", __LOC__ );

	    off_t off = (pageId & ~_poolMask) << _pageBits;
	    off_t limit = off + ((_poolMask + 1) << _pageBits);
        size_t len = (_poolMask + 1) << _pageBits;
	    int prot = PROT_READ | (BLT_ro==_mode ? 0 : PROT_WRITE );
	    if (MAP_FAILED == (pool->_map = (char *)mmap( 0, len, prot, MAP_SHARED, _fd, off ))) {
            __OSS__( "mmap segment " << pageId << " failed" );
            Logger::logDebug( thread, __ss__, __LOC__ );
	        return BLTERR_map;
        }
	    return BLTERR_ok;
	}

	Page* BufferMgr::page( Pool* pool, PageId pageId, const char* thread ) {
        if (BUFFER_MGR_TRACE) Logger::logDebug( thread, "", __LOC__ );

        assert( NULL != pool );

	    uint subpage = (uint)(pageId & _poolMask); // page within mapping
	    Page* page = (Page*)(pool->_map + (subpage << _pageBits));
	    return page;
	}


	void BufferMgr::unpinPool( Pool *pool, const char* thread ) {
        if (BUFFER_MGR_TRACE) Logger::logDebug( thread, "", __LOC__ );
	    __sync_fetch_and_add( &pool->_pin, -1 );
        assert( NULL != pool );
	}
	
	Pool* BufferMgr::pinPool( PageId pageId, const char* thread ) {
        if (BUFFER_MGR_TRACE) Logger::logDebug( thread, "", __LOC__ );

	    // lock hash table chain
	    uint hashIndex = (uint)(pageId >> _segBits) % _hashSize;
	    SpinLatch::spinWriteLock( &_latch[ hashIndex ], thread );
	
	    // look up in hash table
	    Pool* pool = findPool( pageId, hashIndex, thread );
	    if (pool) {
	        __sync_fetch_and_or( &pool->_pin, CLOCK_bit);
	        __sync_fetch_and_add( &pool->_pin, 1);
	        SpinLatch::spinReleaseWrite( &_latch[ hashIndex ], thread );
	        return pool;
	    }
	
	    // allocate a new pool node, and add to hash table
	    uint slot = __sync_fetch_and_add(&_poolCnt, 1);
	
	    if (++slot < _poolMax) {
	        pool = &_pool[ slot ];
	        pool->_slot = slot;
	
	        if (mapSegment( pool, pageId, thread )) {
                __OSS__( "mapSegment " << pageId << " failed" );
                Logger::logError( thread, __ss__, __LOC__ );
	            return NULL;
            }
	
	        linkHash( pool, pageId, hashIndex, thread );
	        SpinLatch::spinReleaseWrite( &_latch[ hashIndex ], thread );
	        return pool;
	    }
	
	    // pool table is full: find best pool entry to evict
	    __sync_fetch_and_add( &_poolCnt, -1 );
	
	    while (true) {
	        uint victim = __sync_fetch_and_add( &_evicted, 1 );
	        victim %= _poolMax;
	        pool = &_pool[ victim ];
	        uint i = (uint)(pool->_basePage >> _segBits) % _hashSize;
	
	        if (!victim) continue;
	
	        // try to get write lock, skip entry if not obtained
	        if (!SpinLatch::spinTryWrite( &_latch[i], thread )) continue;
	
	        //    skip this entry if page is pinned or clock bit is set
	        if (pool->_pin) {
	            __sync_fetch_and_and( &pool->_pin, (ushort)~CLOCK_bit );
	            SpinLatch::spinReleaseWrite( &_latch[i], thread );
	            continue;
	        }
	
	        // unlink victim pool node from hash table
            Pool* node;
	        if ( (node = (Pool *)pool->_hashPrev) ) {
	            node->_hashNext = pool->_hashNext;
            }
	        else if( (node = (Pool *)pool->_hashNext) ) {
	            _hash[i] = node->_slot;
            }
	        else {
	            _hash[i] = 0;
            }
	
	        if ( (node = (Pool *)pool->_hashNext) ) {
	            node->_hashPrev = pool->_hashPrev;
            }
	
	        SpinLatch::spinReleaseWrite( &_latch[i], thread );
	
	        // remove old file mapping
	        munmap( pool->_map, (_poolMask+1) << _pageBits );
	        pool->_map = NULL;
	
	        // create new pool mapping and link into hash table
	        if (mapSegment( pool, pageId, thread )) {
                __OSS__( "mapSegment " << pageId << " failed" );
                Logger::logError( thread, __ss__, __LOC__ );
                return NULL;
            }
	
	        linkHash( pool, pageId, hashIndex, thread );
	        SpinLatch::spinReleaseWrite( &_latch[ hashIndex ], thread );
	        return pool;

	    }   // end while
	}

    /**
    *  place write, read, or parent lock on requested page
    */
    void BufferMgr::lockPage( LockMode mode, LatchSet* set, const char* thread ) {
        if (BUFFER_MGR_TRACE) Logger::logDebug( thread, "", __LOC__ );

        assert( NULL != set );

        switch( mode ) {
        case LockRead:   { SpinLatch::spinReadLock( set->_readwr, thread ); break;  }
        case LockWrite:  { SpinLatch::spinWriteLock( set->_readwr, thread ); break; }
        case LockAccess: { SpinLatch::spinReadLock( set->_access, thread ); break;  }
        case LockDelete: { SpinLatch::spinWriteLock( set->_access, thread ); break; }
        case LockParent: { SpinLatch::spinWriteLock( set->_parent, thread ); break; }
        }
    }
    
    /**
    *  remove write, read, or parent lock on requested page
    */
    void BufferMgr::unlockPage( LockMode mode, LatchSet* set, const char* thread ) {
        if (BUFFER_MGR_TRACE) Logger::logDebug( thread, "", __LOC__ );

        assert( NULL != set );

        switch (mode) {
        case LockRead:   { SpinLatch::spinReleaseRead( set->_readwr, thread ); break;  }
        case LockWrite:  { SpinLatch::spinReleaseWrite( set->_readwr, thread ); break; }
        case LockAccess: { SpinLatch::spinReleaseRead( set->_access, thread ); break;  }
        case LockDelete: { SpinLatch::spinReleaseWrite( set->_access, thread ); break; }
        case LockParent: { SpinLatch::spinReleaseWrite( set->_parent, thread ); break; }
        }
    }

    /**
    * allocate a new page and write page into it
    */
    PageId BufferMgr::newPage( Page* inputPage, const char* thread ) {
        if (BUFFER_MGR_TRACE) Logger::logDebug( thread, "", __LOC__ );
    
        assert( NULL != inputPage );

        // lock allocation page
        SpinLatch::spinWriteLock( _latchMgr->_lock, thread );
    
        // use empty chain first, else allocate empty page
        PageSet set[1];
        int reuse;

        PageId newPage = Page::getid( _latchMgr->_alloc[1]._right );

        if (newPage) {
            if ( (set->_pool = pinPool( newPage, thread )) ) {
                set->_page = page( set->_pool, newPage, thread );
            }
            else {
                return 0;
            }
    
            Page::putid( _latchMgr->_alloc[1]._right, Page::getid( set->_page->_right ) );
            unpinPool( set->_pool, thread );
            reuse = 1;
        } else {
            newPage = Page::getid( _latchMgr->_alloc->_right );
            Page::putid( _latchMgr->_alloc->_right, newPage+1 );
            reuse = 0;
        }
        if (pwrite( _fd, inputPage, _pageSize, newPage << _pageBits) < _pageSize) {
            _err = BLTERR_wrt;
            return 0;
        }
    
        // if writing first page of pool block, zero last page in the block
        if (!reuse && (_poolMask > 0) && 0==(newPage & _poolMask)) {

            // use zero buffer to write zeros
            off_t off = (newPage | _poolMask) << _pageBits;
            if (pwrite( _fd, _zero, _pageSize, off ) < _pageSize ) {
                __OSS__( "write of zero page syserr: " << strerror(errno) );
                Logger::logError( thread, __ss__, __LOC__ );
                return 0;
            }
        }

        // unlock allocation latch and return new page
        SpinLatch::spinReleaseWrite( _latchMgr->_lock, thread );
        return newPage;
    }

    #define LOADPAGE_TRACE  false

    int BufferMgr::loadPage( PageSet* set,
                             const uchar* key,
                             uint keylen,
                             uint level,
                             LockMode inputMode,
                             const char* thread )
    {
        if (BUFFER_MGR_TRACE) Logger::logDebug( thread, "", __LOC__ );
    
        assert( NULL != set );
        assert( NULL != key );

        // start at root of btree and drill down
        PageId pageId = ROOT_page;
        uint drill = 0xff;

        PageId prevPageId = 0;
        LockMode prevMode;
        LatchSet* prevLatch;
        Pool* prevPool;

        do {
            // determine lock mode of drill level
            LockMode mode = (drill == level) ? inputMode : LockRead; 

            set->_latch = _latchMgr->pinLatch( pageId, thread );
            set->_pageNo = pageId;

            // pin page contents
            if ( (set->_pool = pinPool( pageId, thread )) ) {
                set->_page = page( set->_pool, pageId, thread );
            }
            else {
                __OSS__( "pinPool failed on page: " << pageId );
                Logger::logError( thread, __ss__, __LOC__ );
                return 0;
            }

            // obtain access lock using lock chaining with Access mode
            if (pageId > ROOT_page) {
                lockPage( LockAccess, set->_latch, thread );
            }

            // release & unpin parent page
            if (prevPageId) {
                unlockPage( prevMode, prevLatch, thread );
                _latchMgr->unpinLatch( prevLatch, thread );
                unpinPool( prevPool, thread );
                prevPageId = 0;
            }

            // obtain read lock using lock chaining
            lockPage( mode, set->_latch, thread );

            if (set->_page->_free) {
                __OSS__( "expecting free page: " << pageId );
                Logger::logError( thread, __ss__, __LOC__ );
                 _err = BLTERR_struct;
                return 0;
            }

            if (pageId > ROOT_page) {
                unlockPage( LockAccess, set->_latch, thread );
            }

            // re-read and re-lock root after finding root level
            if (set->_page->_level != drill) {
                if (set->_pageNo != ROOT_page) {
                    __OSS__( "level!=drill  on page: " << pageId );
                    Logger::logError( thread, __ss__, __LOC__ );
                    _err = BLTERR_struct;
                    return 0;
                }
            
                drill = set->_page->_level;

                if (inputMode != LockRead && drill == level) {
                    unlockPage( mode, set->_latch, thread );
                    _latchMgr->unpinLatch( set->_latch, thread );
                    unpinPool( set->_pool, thread );
                    continue;
                }
            }

            prevPageId = set->_pageNo;
            prevLatch  = set->_latch;
            prevPool   = set->_pool;
            prevMode   = mode;

            // find key on page at this level, descend to requested level
            if (!set->_page->_kill) {

                uint slot = findSlot( set, key, keylen, thread );

                if (slot) {
                    if (drill == level) return slot;

                    while (Page::slotptr(set->_page, slot)->_dead) {
                        if (slot++ < set->_page->_cnt) {
                            continue;
                        }
                        else {
                            goto slideright;
                        }
                    }

                    pageId = Page::getid( Page::slotptr(set->_page, slot)->_id );
                    drill--;
                    continue;
                }
            }

slideright: //  or slide right into next page
            pageId = Page::getid( set->_page->_right );

        } while (pageId);

        // return error on end of right chain
        _err = BLTERR_struct;
        return 0; 
    }

    /**
    *  find slot in page for given key at a given level
    */
    int BufferMgr::findSlot( PageSet *set, const uchar* key, uint keylen, const char* thread ) {
        if (BUFFER_MGR_TRACE) Logger::logDebug( thread, "", __LOC__ );

        assert( NULL != set );
        assert( NULL != key );

        uint diff;
        uint higher = set->_page->_cnt;
        uint low = 1;
        uint slot;
        uint good = 0;

        // make stopper key an infinite fence value
        if (Page::getid( set->_page->_right )) {
            higher++;
        }
        else {
            good++;
        }

        // low is the lowest candidate; loop ends when they meet
        //  higher is already tested >= the passed key.
        while ((diff = higher - low)) {
            slot = low + (diff >> 1);
            if (BLTKey::keycmp( Page::keyptr(set->_page, slot), key, keylen) < 0) {
                low = slot + 1;
            }
            else {
                higher = slot;
                good++;
            }
        }

        // return zero if key is on right link page
        return (good ? higher : 0);
    }

    /**
    *  Read page from permanent location in BLTIndex file
    */
    int BufferMgr::readPage( Page* page, PageId pageId, const char* thread ) {
        if (BUFFER_MGR_TRACE) Logger::logDebug( thread, "", __LOC__ );

        assert( NULL != page );
        
        off_t off = (pageId << _pageBits);
        uint32_t pageSize = (1 << _pageBits);

        if (pread( _fd, page, pageSize, off ) < pageSize) {
            __OSS__( "Error reading page " << pageId << ", syserr = " << strerror(errno) );
            Logger::logError( thread, __ss__, __LOC__ );
            return BLTERR_read;
        }
        return 0;
    }       
            
    /**         
    *  Write page to permanent location in BLTIndex file.
    *  Clear the dirty bit
    */
    int BufferMgr::writePage( Page* page, PageId pageId, const char* thread ) {
        if (BUFFER_MGR_TRACE) Logger::logDebug( thread, "", __LOC__ );

        assert( NULL != page );

        off_t off = (pageId << _pageBits);
        uint32_t pageSize = (1 << _pageBits);

        page->_dirty = 0;
        if (pwrite( _fd, page, pageSize, off) < pageSize) {
            __OSS__( "Error writing page " << pageId << ", syserr = " << strerror(errno) );
            Logger::logError( thread, __ss__, __LOC__ );
            return BLTERR_wrt;
        }
        return 0;
    }

    /**
    *  return page to free list
    *  page must be delete anad write locked
    */
    void BufferMgr::freePage( PageSet* set, const char* thread ) {
        if (BUFFER_MGR_TRACE) Logger::logDebug( thread, "", __LOC__ );

        assert( NULL != set );

        // lock allocation page
        SpinLatch::spinWriteLock( _latchMgr->_lock, thread );

        // store chain in second right
        Page::putid( set->_page->_right, Page::getid( _latchMgr->_alloc[1]._right ) );
        Page::putid( _latchMgr->_alloc[1]._right, set->_pageNo);
        set->_page->_free = 1;

        // unlock released page
        unlockPage( LockDelete, set->_latch, thread );
        unlockPage( LockWrite, set->_latch, thread );
        _latchMgr->unpinLatch( set->_latch, thread );
        unpinPool( set->_pool, thread );

        // unlock allocation page
        SpinLatch::spinReleaseWrite( _latchMgr->_lock, thread );
    }

    /**
    *
    */
    std::string BufferMgr::decodeLastErr() const {
        switch (_err) {
        case BLTERR_ok: return "ok";
        case BLTERR_notfound: return "not found";
        case BLTERR_struct: return "structural";
        case BLTERR_ovflw: return "overflow";
        case BLTERR_read: return "read";
        case BLTERR_lock: return "lock";
        case BLTERR_hash: return "hash";
        case BLTERR_kill: return "kill";
        case BLTERR_map: return "map";
        case BLTERR_wrt: return "write";
        case BLTERR_eof: return "eof";
        default: return "unrecognized!!?";
        }
    }

    #define LATCHAUDIT_TRACE    false

    /**
    *
    */
    void BufferMgr::latchAudit( const char* thread ) {
        if (BUFFER_MGR_TRACE) Logger::logDebug( thread, "", __LOC__ );

        if (*(uint *)(_latchMgr->_lock)) {
            Logger::logDebug( thread, "Alloc page locked", __LOC__ );
        }
        *(uint *)(_latchMgr->_lock) = 0;
    
        for (ushort idx = 1; idx <= _latchMgr->_latchDeployed; idx++ ) {

            LatchSet* latchSet = &_latchMgr->_latchSets[ idx ];
            PageId pageId = latchSet->_pageId;

            if (*(uint *)latchSet->_readwr ) {
                __OSS__( "latchset " << idx << " rw locked for page " << pageId );
                Logger::logDebug( thread, __ss__, __LOC__ );
            }
            *(uint *)latchSet->_readwr = 0;
    
            if (*(uint *)latchSet->_access ) {
                __OSS__( "latchset " << idx << " access locked for page " << pageId );
                Logger::logDebug( thread, __ss__, __LOC__ );
            }
            *(uint *)latchSet->_access = 0;
    
            if (*(uint *)latchSet->_parent ) {
                __OSS__( "latchset " << idx << " parent locked for page " << pageId );
                Logger::logDebug( thread, __ss__, __LOC__ );
            }
            *(uint *)latchSet->_parent = 0;
    
            if (latchSet->_pin ) {
                __OSS__( "latchset " << idx << " pinned for page " << pageId );
                Logger::logDebug( thread, __ss__, __LOC__ );
                latchSet->_pin = 0;
            }
        }
    
        for (ushort hashidx = 0; hashidx < _latchMgr->_latchHash; hashidx++ ) {
            if (*(uint *)(_latchMgr->_table[hashidx]._latch) ) {
                __OSS__( "hash entry " << hashidx << " locked" );
                Logger::logDebug( thread, __ss__, __LOC__ );
            }
            *(uint *)(_latchMgr->_table[hashidx]._latch) = 0;
    
            uint idx = _latchMgr->_table[hashidx]._slot;
            if (idx) {
                LatchSet* latchSet;
                do {
                    latchSet = &_latchMgr->_latchSets[ idx ];
                    PageId pageId = latchSet->_pageId;

                    if (*(uint *)latchSet->_busy ) {
                        __OSS__( "latchset " << idx << " busy locked for page " << pageId );
                        Logger::logDebug( thread, __ss__, __LOC__ );
                    }
                    *(uint *)latchSet->_busy = 0;
                    if (latchSet->_hash != hashidx ) {
                        __OSS__( "latchset " << idx << " wrong hashidx " );
                        Logger::logDebug( thread, __ss__, __LOC__ );
                    }
                    if (latchSet->_pin ) {
                        __OSS__( "latchset " << idx << " pinned for page " << pageId );
                        Logger::logDebug( thread, __ss__, __LOC__ );
                    }
                } while ((idx = latchSet->_next));
            }
        }
    
        PageId next = _latchMgr->_nlatchPage + LATCH_page;
        PageId pageNo = LEAF_page;
        Page* _frame = (Page *)malloc( _pageSize );
    
        while (pageNo < Page::getid(_latchMgr->_alloc->_right)) {
            pread( _fd, _frame, _pageSize, pageNo << _pageBits );
            if (!_frame->_free) {
                for (uint idx = 0; idx++ < _frame->_cnt - 1; ) {
                    BLTKey* key = Page::keyptr(_frame, idx+1);
                    if (BLTKey::keycmp( Page::keyptr(_frame, idx), key->_key, key->_len ) >= 0) {
                        __OSS__( "page " << pageNo << " idx" << idx << " out of order" );
                        Logger::logDebug( thread, __ss__, __LOC__ );
                    }
                }
            }
    
            if (pageNo > LEAF_page) next = pageNo + 1;
            pageNo = next;
        }
    }

}   // namespace mongo