//@file bltree.h
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
*  This module contains derived code.   The original
*  copyright notice is as follows:
*
*    This work, including the source code, documentation
*    and related data, is placed into the public domain.
*  
*    The orginal author is Karl Malbrain (malbrain@cal.berkeley.edu)
*  
*    THIS SOFTWARE IS PROVIDED AS-IS WITHOUT WARRANTY
*    OF ANY KIND, NOT EVEN THE IMPLIED WARRANTY OF
*    MERCHANTABILITY. THE AUTHOR OF THIS SOFTWARE,
*    ASSUMES _NO_ RESPONSIBILITY FOR ANY CONSEQUENCE
*    RESULTING FROM THE USE, MODIFICATION, OR
*    REDISTRIBUTION OF THIS SOFTWARE.
*/

#pragma once

#ifndef STANDALONE
#include "mongo/db/storage/mmap_v1/bltree/common.h"
#include "mongo/db/storage/mmap_v1/bltree/blterr.h"
#include "mongo/db/storage/mmap_v1/bltree/bltkey.h"
#include "mongo/db/storage/mmap_v1/bltree/page.h"
#else
#include "common.h"
#include "blterr.h"
#include "bltkey.h"
#include "page.h"
#endif

namespace mongo {
    /**
    *  The page is allocated from low and high ends.  The key offsets and
    *  PageNo's are allocated from the bottom, while the text of the key is
    *  allocated from the top.  When the two areas meet, the page is split.
    *
    *  A key consists of a length byte, two bytes of index number (0 - 65534),
    *  and up to 253 bytes of key value.  Duplicate keys are discarded.
    *  Associated with each key is a 48 bit docid, or any other value desired.
	*
    *  The bltindex root is always located at page 1.  The first leaf page of
    *  level zero is always located on page 2.
	*
    *  The bltindex pages are linked with next pointers to facilitate
    *  enumerators, and provide for concurrency.
	*
    *  When the root page fills, it is split in two and the tree height is
    *  raised by a new root at page one with two keys.
	*
    *  Deleted keys are marked with a 'dead' bit until page cleanup. The fence
    *  key for a node is always present
	*
    *  Groups of pages called segments from the bltindex are optionally cached
    *  with a memory mapped pool.  A hash table is used to keep track of the
    *  cached segments.  This behaviour is controlled by the cache block size
    *  parameter to BLT::open.
	*
    *  To achieve maximum concurrency one page is locked at a time as the tree
    *  is traversed to find leaf key in question.  The right page numbers are
    *  used in cases where the page is being split, or consolidated.
	*
    *  Page 0 is dedicated to locks for new page extensions, and chains empty
    *  pages together for reuse.
	*
    *  The ParentModification lock on a node is obtained to serialize posting
    *  or changing the fence key for a node.
	*
    *  Empty pages are chained together through the ALLOC page and reused.
	*
    *  Access macros to address slot and key values from the page Page slots
    *  use 1 based indexing.
    */
	
    class LatchMgr;
    class LatchHashEntry;
    class Page;
    class BufferMgr;

    class BLTree {
    public:
        /**
        *  Open: allocate BLTree.
        *  @param mgr  -  
        */
        static BLTree* create( BufferMgr* mgr, const char* threadName );

        /**
        *  Close: release memory.
        */
        void close();

        /**
        *  Insert new key,value into the btree at given level.
        *  @param key  -  key to insert
        *  @param keyLen  -  length of key to insert
        *  @param doc  -  document to insert
        *  @param doclen  -  document length
        *  @param level  -  tree level to insert into 
        *  @return BLTERR::OK if no error, otherwise appropriate error
        */
        BLTERR insertKeyVal( const uchar* key,
                             uint keyLen,
                             const uchar* val,
                             uint valLen,
                             uint level );

        /**
        *  Insert new key,value into the btree at given level.
        *  @param key  -  key to insert
        *  @param keyLen  -  length of key to insert
        *  @param pageNo  -  page number to insert (as doc)
        *  @param level  -  tree level to insert into 
        *  @return BLTERR::OK if no error, otherwise appropriate error
        */
        BLTERR insertKeyVal( const uchar* key,
                             uint keyLen,
                             PageNo pageNo,
                             uint level );

        /**
        *  find and delete key on page by marking delete flag bit:
        *   if page becomes empty, delete it from the btree
        *  @param key  -  
        *  @param keyLen  -  
        *  @param level  -  
        *  @return
        */
        BLTERR deleteKey( const uchar* key,
                          uint keyLen,
                          uint level );

        /**
        *  Find key in leaf level and return pageNo.
        *  @param inKey  -   key to find 
        *  @param inKeyLen  -  length of search key
        *  @param outVal  -  output value
        *  @param outValMaxLen  -  maximum output length
        *  @return output value length
        */
        int findKey( const uchar* inKey,
                     uint inKeyLen,
                     uchar* outVal,
                     uint outValMaxLen );

        /**
        *  Cache page of keys into cursor: return starting slot for given key
        *  @param key  -  
        *  @param keyLen  -  
        *  @return
        */
        uint startKey( const uchar* key,
                       uint keyLen );

        /**
        *  Return next slot for cursor page, or slide cursor right into next page.
        *  @param slot  -  
        *  @return
        */
        uint nextKey( uint slot );
        
        /**
        *  Print audit of index latch tables.
        */
        void latchAudit();

        // accessors
        BLTERR getLastError() const { return _err; }
	    Page* getFrame() const { return _frame; }
        const char* getThread() const { return _thread; }

    protected:
        /**
        *  Root has a single child: collapse a level from the tree.
        *  @param root  -  
        *  @return
        */
        BLTERR collapseRoot( PageSet* root );

        /**
        *  Split the root and raise the height of the btree.
        *  @param leftkey  -  
        *  @param docId  -  
        *  @return
        */
        BLTERR splitRoot( PageSet* root,
                          const uchar* leftKey,
                          PageNo pageNo );

        /**
        *  Split already locked full node: return unlocked.
        *  @return
        */
        BLTERR splitPage( PageSet* set );

        /**
        *  Check page for space available, clean if necessary.
        *  @param page  -  page to be compressed 
        *  @param keyLen  -  length of key trying to insert
        *  @param valLen  -  length of value trying to insert
        *  @param slot  -  slot where we are inserting new key
        *  @return  0 if page needs splitting, >0 if new slot value
        */
        uint cleanPage( Page* page, uint keyLen, uint valLen, uint slot );

        /**
        *  Fence key was deleted from a page: push new fence value upwards.
        *  @param docId  -  
        *  @param level  -  
        *  @return
        */
        BLTERR fixFenceKey( PageSet* set, uint level );

    protected:
        const char* _thread;        // thread name
	    BufferMgr* _mgr;            // buffer pool manager
	    Page* _cursor;              // cached frame for start/next (not mapped)
	    Page* _frame;               // spare frame for the page split (not mapped)
	    PageNo _cursorPage;         // current cursor page number    
	    uchar* _mem;                // frame, cursor, page memory buffer
	    int _found;                 // last delete or insert was found
        BLTERR _err;                // most recent error
    };
 
}   // namespace mongo

