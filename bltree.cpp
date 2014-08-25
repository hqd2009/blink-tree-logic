//@file bltree.cpp
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

#ifndef STANDALONE
#include "mongo/platform/basic.h"
#include "mongo/util/assert_util.h"
#include "mongo/db/storage/mmap_v1/bltree/bltree.h"
#include "mongo/db/storage/mmap_v1/bltree/common.h"
#include "mongo/db/storage/mmap_v1/bltree/latchmgr.h"
#include "mongo/db/storage/mmap_v1/bltree/logger.h"
#include "mongo/db/storage/mmap_v1/bltree/bufmgr.h"
#include "mongo/db/storage/mmap_v1/bltree/page.h"
#else
#include "bltree.h"
#include "common.h"
#include "latchmgr.h"
#include "logger.h"
#include "bufmgr.h"
#include "page.h"
#include <assert.h>
#endif

#include <fcntl.h>
#include <iostream>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <sstream>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#define BLTINDEX_TRACE  false

namespace mongo {

    /**
    *    This is an implementation of the Lehman-Yao B-Link tree data type.
    *
    *    [LH] Lehman, Philip and Yao, S. Bing: "Efficient Locking for Concurrent
    *         Operations on B-Trees, "ACM Transactions on Database Systems,
    *         Vol 6, No 4, 1981, pp. 650-670.
    *
    *    Leaf nodes are at level 0 and contain docs inline.
    *    Internal nodes include only separator keys and child page locators.
    *
    *    Range queries a <= k < b are resolved by locating the (page,slot)
    *    of min { (k,loc) : a <= k }, and max { (k,loc) : k < b }, then
    *    traversing leaf nodes from min (page,slot) to max (page,slot).
    *
    *    Nodes are managed with fine-grained locking, one LatchSet per page.
    *    A LatchSet includes three independent latches for managing
    *
    *        1. access-intent / delete
    *        2. read / write
    *        3. parent fence key update
    *
    *    See the documentation in latchmgr.h for details, specifically, the
    *    compatibility matrix for each latch.  The sets are independent, so you can
    *    hold an access-intent and a read latch at the same time, for instance.
    *
    *    In the original Lehman-Yao paper, insertion maintained a stack of nodes
    *    traversed, and then popped the stack to determine where to place new separator
    *    keys in the event of a node split.  If an ancestor node were split by some
    *    other thread, then the current thread may need to traverse a right sibling
    *    link to find the node where the new separator key should go.  Hence the name
    *    'B-Link' tree.  In our implementation, no stack is maintained.  Instead,
    *    the insertKey method recursively call itself with the separator key and a
    *    'level' parameter == L+1 == parent level.  This entails retraversing from
    *    the root to the parent node.
    *
    *    [LY] did not provide an algorithm for deletion and reuse of vacant nodes.
    *    But
    *
    *    [LS] Lanin, Vladimir and Shasha, Dennis: "A symmetric concurrent B-tree
    *         algorithm," Proc. ACM 86, 1986, pp. 380-389.
    *
    *    does provides a proposal for such an algorithm.  We implement an algorithm
    *    similar to [LS] by setting tombstones and decrementing a count of
    *    active keys.  Once the active key count decrements to 0, the node is empty
    *    and may be reclaimed.  (The implementation does not currently attempt to
    *    consolidate sparse adjacent non-empty siblings.)  The empty node's right
    *    sibling keys are transferred into the empty node.  The right sibling is then
    *    spliced out of the sibling list and moved to the head of the free list.
    *    This is done because the sibling list is singly linked, and we have no
    *    predecesor link.  In the case where the empty node is the last node on
    *    given level, and has no right sibling, it is left in place and is available
    *    to contain new keys with curMax < key <= +infty.  During the deletion,
    *    the empty node's right sibling pointer is reused as a forwarding pointer
    *    to the left sibling, allowing concurrent read access though (the now obsolete)
    *    empty right sibling.
    *
    *
    */

    void BLTree::close() {
        if (BLTINDEX_TRACE) Logger::logDebug( _thread, "", __LOC__ );
        if (_mem) free(_mem);
    }
    
    /**
    *  Open BLTree access method.
    *  @param mgr  -  
    *  @param log  -  
    *  @return
    */
    BLTree* BLTree::create( BufferMgr* mgr, const char* thread ) {
        if (BLTINDEX_TRACE) Logger::logDebug( thread, "", __LOC__ );

        uassert( -1, "NULL == mgr", NULL != mgr );
        uassert( -1, "NULL == thread", NULL != thread );

        BLTree* blt = new BLTree();
        memset( blt, 0, sizeof(*blt) );
        blt->_thread    = thread;
        blt->_mgr       = mgr;
        blt->_mem       = (uchar *)malloc( 2 * mgr->getPageSize() );
        blt->_frame     = (Page*)(blt->_mem);
        blt->_cursor    = (Page*)(blt->_mem + mgr->getPageSize() );
        return blt;
    }
    
    /**
    *  a fence key was deleted from a page: push new fence value upwards
    *  @param set -  
    *  @param level -  
    *  @return
    */
    BLTERR BLTree::fixFenceKey( PageSet* set, uint level ) {
        if (BLTINDEX_TRACE) Logger::logDebug( _thread, "", __LOC__ );

        uassert( -1, "NULL == set", NULL != set );

        uchar leftKey[256];
        uchar rightKey[256];
        PageNo pageNo;
        BLTKey* key;
    
        // remove the old fence value
        key = Page::keyptr( set->_page, set->_page->_cnt );    // i.e. pull out fence key
        memcpy( rightKey, key, key->_len + 1);                // save it
        memset( Page::slotptr( set->_page, set->_page->_cnt-- ), 0, sizeof(Slot) );        // clear out fence slot
        set->_page->_dirty = 1;
    
        key = Page::keyptr( set->_page, set->_page->_cnt);
        memcpy( leftKey, key, key->_len + 1 );
        pageNo = set->_pageNo;
    
        _mgr->lockPage( LockParent, set->_latch, _thread );
        _mgr->unlockPage( LockWrite, set->_latch, _thread );
    
        // insert new (now smaller) fence key / upstairs
        if (insertKeyVal( leftKey+1, *leftKey, pageNo, level+1 )) {
            return _err;
        }

        // delete old fence key / upstairs
        if (deleteKey( rightKey+1, *rightKey, level+1 )) {
            return _err;
        }
    
        _mgr->unlockPage( LockParent, set->_latch, _thread );
        _mgr->unpinLatch( set->_latch, _thread );
        _mgr->unpinPoolEntry( set->_pool, _thread );
        return BLTERR_ok;
    }
    
    /**
    *  root has a single child: collapse a level from the tree.
    *  @param root
    *  @return
    */
    BLTERR BLTree::collapseRoot( PageSet* root ) {
        if (BLTINDEX_TRACE) Logger::logDebug( _thread, "", __LOC__ );

        uassert( -1, "NULL == root", NULL != root );

        PageSet child[1];
    
        // collapse a linear chain from root
        do {
            // find the only child entry
            uint idx;
            for (idx = 0; idx++ < root->_page->_cnt; ) {
                if (!Page::slotptr(root->_page, idx)->_dead) break;
            }
        
            // and promote as new root contents
            child->_pageNo = BLTVal::getPageNo( Page::valptr( root->_page, idx )->_value );

            child->_latch = _mgr->pinLatch( child->_pageNo, _thread );
            _mgr->lockPage( LockDelete, child->_latch, _thread );    // waits on AI locks
            _mgr->lockPage( LockWrite, child->_latch, _thread );    // prepare to modify
        
            if ( (child->_pool = _mgr->pinPoolEntry( child->_pageNo, _thread )) ) {
                child->_page = _mgr->page( child->_pool, child->_pageNo, _thread );
            }
            else {
                return _err;
            }
            memcpy( root->_page, child->_page, _mgr->getPageSize() );
            _mgr->freePage( child, _thread );
    
        } while (root->_page->_level > 1 && root->_page->_act == 1 /* i.e. single child */ );
    
        _mgr->unlockPage( LockWrite, root->_latch, _thread );
        _mgr->unpinLatch( root->_latch, _thread );
        _mgr->unpinPoolEntry( root->_pool, _thread );
        return BLTERR_ok;
    }
    
    /**
    *  find and delete key on page by marking delete flag bit,
    *  if page becomes empty, delete it from the btree.
    *
    *  Note: emptied block is one to the right, if any.
    *              It's a singly linked list, we access one to the right,
    *              move all the keys into the current (empty) node,
    *              and delete the guy to the right.  If no node to the
    *              right then we skip this step, leave an empty node,
    *              available for use in the tree - for cuurent_max < key <= +infty
    *      
    */
    BLTERR BLTree::deleteKey( const uchar* inputKey, uint inputKeyLen, uint level ) {
        if (BLTINDEX_TRACE) Logger::logDebug( _thread, "", __LOC__ );

        uassert( -1, "NULL == inputKey", NULL != inputKey );

        uchar lowerFence[256];
        uchar higherFence[256];
        PageSet set[1];
        PageSet right[1];
        BLTKey* key;
    
        uint slot = _mgr->loadPage( set, inputKey, inputKeyLen, level, LockWrite, _thread );
        if (slot) {
            key = Page::keyptr( set->_page, slot );
        }
        else {
            return _err;
        }
    
        // are we deleting a fence slot?
        uint fence = (slot == set->_page->_cnt);
    
        // if key is found delete it, otherwise ignore request
        uint dirty = 0;
        uint found = !BLTKey::keycmp( key, inputKey, inputKeyLen );
        if (found) {
            found = Page::slotptr( set->_page, slot )->_dead;
            if (0==found) {
                dirty = Page::slotptr( set->_page, slot )->_dead = 1;
                set->_page->_dirty = 1;
                set->_page->_act--;
    
                // collapse empty slots
                uint idx;
                while ( (idx = set->_page->_cnt - 1) ) {
                    if (!Page::slotptr( set->_page, idx )->_dead ) break;
                    *Page::slotptr( set->_page, idx ) = *Page::slotptr( set->_page, idx + 1 );
                    memset( Page::slotptr( set->_page, set->_page->_cnt--), 0, sizeof(Slot) );
                }
            }
        }
    
        // did we delete a fence key in an upper level (internal node)?
        if (dirty && level && set->_page->_act && fence) {
            if (fixFenceKey( set, level )) {
                return _err;
            }
            else {
                _found = found;
                return BLTERR_ok;
            }
        }
    
        // is this a collapsed root?   Note: root is never below level 1
        if (level > 1 && set->_pageNo == ROOT_page && set->_page->_act == 1) {
            if (collapseRoot( set )) {
                return _err;
            }
            else {
                _found = found;
                return BLTERR_ok;
            }
        }
    
        // return if page is not empty
        if (set->_page->_act) {
            _mgr->unlockPage( LockWrite, set->_latch, _thread );
            _mgr->unpinLatch( set->_latch, _thread );
            _mgr->unpinPoolEntry( set->_pool, _thread );
            _found = found;
            return BLTERR_ok;
        }
    
        // cache copy of fence key to post in parent
        key = Page::keyptr( set->_page, set->_page->_cnt);
        memcpy( lowerFence, key, key->_len + 1 );

        // obtain lock on right page
        right->_pageNo = set->_page->_right;
        right->_latch = _mgr->pinLatch( right->_pageNo, _thread );
        _mgr->lockPage( LockWrite, right->_latch, _thread );

        // pin page contents
        right->_pool = _mgr->pinPoolEntry( right->_pageNo, _thread );
        if (right->_pool) {
            right->_page = _mgr->page( right->_pool, right->_pageNo, _thread );
        }
        else {
            return BLTERR_ok;
        }
    
        if (right->_page->_kill) {
            return (_err = BLTERR_struct);
        }
    
        // pull contents of right peer into our empty page
        memcpy( set->_page, right->_page, _mgr->getPageSize() );
    
        // cache copy of key to update
        key = Page::keyptr( right->_page, right->_page->_cnt );
        memcpy( higherFence, key, key->_len + 1 );
    
        // mark right page deleted: point it to left page until we can post parent updates

        right->_page->_right = set->_pageNo;
        right->_page->_kill = 1;
    
        _mgr->lockPage( LockParent, right->_latch, _thread );
        _mgr->unlockPage( LockWrite, right->_latch, _thread );
        _mgr->lockPage( LockParent, set->_latch, _thread );
        _mgr->unlockPage( LockWrite, set->_latch, _thread );
    
        // redirect higher key directly to our new node contents
        if (insertKeyVal( higherFence+1, *higherFence, set->_pageNo, level+1 )) {
            return _err;
        }
    
        // delete old lower key to our node
        if (deleteKey( lowerFence+1, *lowerFence, level+1 )) {
            return _err;
        }
    
        // obtain delete and write locks to right node
        _mgr->unlockPage( LockParent, right->_latch, _thread );
        _mgr->lockPage( LockDelete, right->_latch, _thread );
        _mgr->lockPage( LockWrite, right->_latch, _thread );
        _mgr->freePage( right, _thread );
    
        _mgr->unlockPage( LockParent, set->_latch, _thread );
        _mgr->unpinLatch( set->_latch, _thread );
        _mgr->unpinPoolEntry( set->_pool, _thread );
        _found = found;
        return BLTERR_ok;
    }
    
    /**
    *  find key in leaf level and return pageNo
    *  @return - the length of value (or valueMax),
    *            or -1 = not found
    */
    int BLTree::findKey( const uchar* inKey,
                         uint inKeyLen,
                         uchar* outVal,
                         uint outValMaxLen )
    {
        if (BLTINDEX_TRACE) Logger::logDebug( _thread, "", __LOC__ );

        uassert( -1, "NULL == inKey", NULL != inKey );

        PageSet set[1];
        BLTKey* key;
        int outValLen = 0;
    
        uint slot = _mgr->loadPage( set, inKey, inKeyLen, 0, LockRead, _thread );
        if (slot) {
            key = Page::keyptr( set->_page, slot );
        }
        else {
            Logger::logDebug( _thread, "loadPage failed", __LOC__ );
            return 0;
        }
    
        // if key exists, load return value, otherwise return -1 (not found)
        if (slot <= set->_page->_cnt ) {
            if (!BLTKey::keycmp( key, inKey, inKeyLen )) {
                BLTVal* val = Page::valptr( set->_page, slot );
                outValLen = (outValMaxLen > val->_len ? val->_len : outValMaxLen); 
                memcpy( outVal, val, outValLen );
            }
            else {
                Logger::logDebug( _thread, "keycmp failed", __LOC__ );
                return -1;
            }
        }
    
        _mgr->unlockPage( LockRead, set->_latch, _thread );
        _mgr->unpinLatch( set->_latch, _thread );
        _mgr->unpinPoolEntry( set->_pool, _thread );
        return outValLen;
    }
    
    /**
    *  Check page for space available, clean if necessary.
    *  @return 0 - page needs splitting, >0  new slot value
    */
    uint BLTree::cleanPage( Page* page,
                            uint keyLen,
                            uint valLen,
                            uint slot )
    {
        if (BLTINDEX_TRACE) Logger::logDebug( _thread, "", __LOC__ );

        uassert( -1, "NULL == page", NULL != page );

        uint nxt = _mgr->getPageSize();
        uint idx = 0;
        uint max = page->_cnt;
        uint newslot = max;
        BLTKey* key;
        BLTVal* val;
    
        // check if room for one more slot + the new key
        if (page->_min >= (max+1)*sizeof(Slot) + sizeof(*page) + keyLen + 1 + valLen + 1) {
            return slot;
        }
    
        // skip cleanup if nothing to reclaim
        if (!page->_dirty ) return 0;
    
        memcpy( _frame, page, _mgr->getPageSize() );
    
        // skip page info and set rest of page to zero
        memset( page+1, 0, _mgr->getPageSize() - sizeof(*page) );
        page->_dirty = 0;
        page->_act = 0;
    
        // try cleaning up page first by removing deleted keys
        uint cnt = 0;
        while (cnt++ < max) {
            if (cnt == slot) newslot = idx + 1;
            if (cnt < max && Page::slotptr(_frame, cnt)->_dead) continue;
    
            // copy the key
            key = Page::keyptr(_frame, cnt);
            nxt -= key->_len + 1;
            memcpy( (uchar *)page + nxt, key, key->_len + 1 );
    
            // copy the value
            val = Page::valptr(_frame, cnt);
            nxt -= val->_len + 1;
            ((uchar*)page)[nxt] = val->_len;
            memcpy( (uchar *)page + nxt + 1, val, val->_len );
    
            // copy slot
            Page::slotptr(page, idx)->_off = nxt;
            if (!(Page::slotptr(page, idx)->_dead = Page::slotptr(_frame, cnt)->_dead)) {
                page->_act++;
            }
        }
    
        page->_min = nxt;
        page->_cnt = idx;
    
        // see if page has enough space now, or does it need splitting?
        if (page->_min >= (idx+1) * sizeof(Slot) + sizeof(*page) + keyLen + 1 + valLen + 1) {
            return newslot;
        }
    
        return 0;
    }
    
    /**
    *  split the root and raise the height of the btree
    */
    BLTERR BLTree::splitRoot( PageSet* root, const uchar* leftKey, PageNo pageNo2) {
        if (BLTINDEX_TRACE) Logger::logDebug( _thread, "", __LOC__ );

        uassert( -1, "NULL == root", NULL != root );
        uassert( -1, "NULL == leftKey", NULL != leftKey );

        uint nxt = _mgr->getPageSize();
        PageNo left;
        uchar value[IdLength];
    
        //  Obtain an empty page to use, and copy the current
        //  root contents into it, e.g. lower keys
        if (!(left = _mgr->newPage( root->_page, _thread ))) {
            return _err;
        }
    
        // preserve the page info at the bottom
        // of higher keys and set rest to zero
    
        memset(root->_page+1, 0, _mgr->getPageSize() - sizeof(*root->_page));
    
        // insert fence key of page with smaller keys as first key of new root page
        nxt -= IdLength + 1;
        BLTVal::putPageNo( value, left );
        ((uchar *)root->_page)[nxt] = IdLength;
        memcpy( (uchar *)root->_page + nxt + 1, value, IdLength );

        nxt -= *leftKey + 1;
        memcpy( (uchar *)root->_page + nxt, leftKey, *leftKey + 1 );
        Page::slotptr(root->_page, 1)->_off = nxt;
        
        // insert stopper key on new root page and increase the root height
        nxt -= 3 + IdLength + 1;
        ((uchar *)root->_page)[nxt] = 2;
        ((uchar *)root->_page)[nxt+1] = 0xff;
        ((uchar *)root->_page)[nxt+2] = 0xff;

        BLTVal::putPageNo( value, pageNo2 );
        ((uchar *)root->_page)[nxt+3] = IdLength;
        memcpy( (uchar*)root->_page + nxt + 4, value, IdLength );
        Page::slotptr(root->_page, 2)->_off = nxt;
    
        root->_page->_right = 0;
        root->_page->_min = nxt;        // reset lowest used offset and key count
        root->_page->_cnt = 2;
        root->_page->_act = 2;
        root->_page->_level++;
    
        // release and unpin root
        _mgr->unlockPage( LockWrite, root->_latch, _thread );
        _mgr->unpinLatch( root->_latch, _thread );
        _mgr->unpinPoolEntry( root->_pool, _thread );
        return BLTERR_ok;
    }
    
    /**
    *  split already locked full node
    *  return unlocked.
    */
    BLTERR BLTree::splitPage( PageSet* set ) {
        if (BLTINDEX_TRACE) Logger::logDebug( _thread, "", __LOC__ );

        uassert( -1, "NULL == set", NULL != set );

        uchar fenceKey[256];
        uchar rightKey[256];
        PageSet right[1];
    
        //  split higher half of keys to _frame
        memset( _frame, 0, _mgr->getPageSize() );
        uint max = set->_page->_cnt;
        uint cnt = max / 2;
        uint idx = 0;
        uint nxt = _mgr->getPageSize();
        uint level = set->_page->_level;

        BLTKey* key( NULL );
        BLTVal* val( NULL );
   
        while (cnt++ < max) {
            val = Page::valptr( set->_page, cnt );
            nxt -= val->_len + 1;
            ((uchar *)_frame)[nxt] = val->_len;
            memcpy ((uchar *)_frame + nxt + 1, val->_value, val->_len);

            key = Page::keyptr(set->_page, cnt);
            nxt -= key->_len + 1;
            memcpy( (uchar *)_frame + nxt, key, key->_len + 1 );

            Page::slotptr(_frame, ++idx)->_off = nxt;

            if (!(Page::slotptr(_frame, idx)->_dead = Page::slotptr(set->_page, cnt)->_dead)) {
                _frame->_act++;
            }

        }
    
        // remember existing fence key for new page to the right
        memcpy( rightKey, key, key->_len + 1 );

        _frame->_bits = _mgr->getPageBits();
        _frame->_min = nxt;
        _frame->_cnt = idx;
        _frame->_level = level;
    
        // link right node
        if (set->_pageNo > ROOT_page) {
            _frame->_right = set->_page->_right;
        }
    
        // get new free page and write higher keys to it
        if (!(right->_pageNo = _mgr->newPage( _frame, _thread ))) {
            return _err;
        }
    
        // update lower keys to continue in old page
        memcpy( _frame, set->_page, _mgr->getPageSize() );
        memset( set->_page+1, 0, _mgr->getPageSize() - sizeof(*set->_page) );
        nxt = _mgr->getPageSize();
        set->_page->_dirty = 0;
        set->_page->_act = 0;
        cnt = 0;
        idx = 0;
    
        // assemble page of smaller keys
        while (cnt++ < max / 2) {

            val = Page::valptr( _frame, cnt );
            nxt -= val->_len + 1;
            ((uchar *)set->_page)[nxt] = val->_len;
            memcpy( (uchar *)set->_page + nxt + 1, val->_value, val->_len );

            key = Page::keyptr( _frame, cnt );
            nxt -= key->_len + 1;
            memcpy( (uchar *)set->_page + nxt, key, key->_len + 1 );

            Page::slotptr( set->_page, idx )->_off = nxt;
            set->_page->_act++;
        }
    
        // remember fence key for smaller page
        memcpy( fenceKey, key, key->_len + 1 );

        set->_page->_right = right->_pageNo;
        set->_page->_min = nxt;
        set->_page->_cnt = idx;
    
        // if current page is the root page, split it
        if (set->_pageNo == ROOT_page) {
            return splitRoot( set, fenceKey, right->_pageNo );
        }
    
        // insert new fences in their parent pages
        right->_latch = _mgr->pinLatch( right->_pageNo, _thread );
        _mgr->lockPage( LockParent, right->_latch, _thread );
        _mgr->lockPage( LockParent, set->_latch, _thread );
        _mgr->unlockPage( LockWrite, set->_latch, _thread );
    
        // insert new fence for reformulated left block of smaller keys
        if (insertKeyVal( fenceKey+1, *fenceKey, set->_pageNo, level+1 )) {
            return _err;
        }
    
        // switch fence for right block of larger keys to new right page
        if (insertKeyVal( rightKey+1, *rightKey, right->_pageNo, level+1 )) {
            return _err;
        }
    
        _mgr->unlockPage( LockParent, set->_latch, _thread );
        _mgr->unpinLatch( set->_latch, _thread );
        _mgr->unpinPoolEntry( set->_pool, _thread );
        _mgr->unlockPage( LockParent, right->_latch, _thread );
        _mgr->unpinLatch( right->_latch, _thread );

        return BLTERR_ok;
    }

    #define INSERT_TRACE    false

    /**
    *  Insert new (key,val) into the btree
    */
    BLTERR BLTree::insertKeyVal( const uchar* inputKey,
                                 uint inputKeyLen,
                                 const uchar* val,
                                 uint valLen,
                                 uint level )
    {
        if (BLTINDEX_TRACE) Logger::logDebug( _thread, "", __LOC__ );

        uassert( -1, "NULL == inputKey", NULL != inputKey );

        PageSet set[1];
        uint slot;
        BLTKey* key;
        int reuse = 0;
    
        while (true) {

            // find the page (returned in 'set') and slot within page for this key
            slot = _mgr->loadPage( set, inputKey, inputKeyLen, level, LockWrite, _thread );

            if (INSERT_TRACE) {
                __OSS__( "(pageNo,slot) = (" << set->_pageNo << ',' << slot << ')' );
                Logger::logDebug( _thread, __ss__, __LOC__ );
            }

            if (slot) {
                key = Page::keyptr(set->_page, slot);
            }
            else {
                if (!_err) _err = BLTERR_ovflw;
                return _err;
            }

            uassert( -1, "NULL == key", NULL != key );
    
            if (INSERT_TRACE) {
                __OSS__( "key = " << key->toString() );
                Logger::logDebug( _thread, __ss__, __LOC__ );
            }

            // if key already exists, update id and return
            if ( (reuse = !BLTKey::keycmp( key, inputKey, inputKeyLen )) ) {

                BLTVal* val = Page::valptr( set->_page, slot );
                if (val) {
                    Slot* slotPtr = Page::slotptr( set->_page, slot );
                    if (slotPtr->_dead) set->_page->_act++;
                    slotPtr->_dead = 0;
                    val->_len = valLen;
                    memcpy( val->_value, val, valLen );
                 
                    _mgr->unlockPage( LockWrite, set->_latch, _thread );
                    _mgr->unpinLatch( set->_latch, _thread );
                    _mgr->unpinPoolEntry( set->_pool, _thread );
                    return BLTERR_ok;
                }
                else {
                    Slot* slotPtr = Page::slotptr( set->_page, slot );
                    if (!slotPtr->_dead) set->_page->_act++;
                    slotPtr->_dead = 1;
                    set->_page->_dirty = 1;
                }
            }
    
            // check if page has enough space
            if ((slot = cleanPage( set->_page, inputKeyLen, valLen, slot)) ) break;
            if (splitPage( set )) return _err;
        }
    
        // calculate next available slot and copy key and value into page. backup by
        // len + 1, write one byte of length and then len bytes of key or value.
        // The key storage grows from high to low addresses on the page, the slot
        // storage grows from low to high addresses.  Page format:
        //
        //                                  page->_min
        //                                  v
        //    [ slot|slot| .. FREE SPACE .. |len,key:len,val|len,key:len,val]
        //    ^                                                             ^
        //    low address                                        high address
        //
        set->_page->_min -= valLen + 1;         // reset lowest used offset
        ((uchar*)set->_page)[set->_page->_min] = valLen;
        memcpy( (uchar*)set->_page + set->_page->_min + 1, val, valLen );

        set->_page->_min -= inputKeyLen + 1;    // reset lowest used offset
        ((uchar *)set->_page)[set->_page->_min] = inputKeyLen;  // 256 byte max length
        memcpy( (uchar *)set->_page + set->_page->_min + 1, inputKey, inputKeyLen );

        // check for an empty slot
        uint idx;
        for (idx = slot; idx < set->_page->_cnt; idx++) {
          if (Page::slotptr(set->_page, idx)->_dead ) break;
        }
    
        // if no emtpy slot: add to end, bump the slot count
        if (!reuse && idx == set->_page->_cnt ) {
            idx++;
            set->_page->_cnt++;
        }

        // bump the count of active keys
        set->_page->_act++;
    
        // linear insertion algorithm: move all the slot pointers down by one
        // until we reach either the previous discovered empty slot or the end
        // of the slot array.

        for (; idx > slot; --idx) {
            *Page::slotptr(set->_page, idx) = *Page::slotptr(set->_page, idx - 1);
        }
    
        // insert new key data into vacant slot
        Slot* slotPtr = Page::slotptr(set->_page, slot);
        slotPtr->_off  = set->_page->_min;
        slotPtr->_dead = 0;
    
        // unlock and return
        _mgr->unlockPage( LockWrite, set->_latch, _thread );
        _mgr->unpinLatch( set->_latch, _thread );
        _mgr->unpinPoolEntry( set->_pool, _thread );
        return BLTERR_ok;
    }
    
    BLTERR BLTree::insertKeyVal( const uchar* inputKey,
                                 uint inputKeyLen,
                                 PageNo pageNo,
                                 uint level )
    {
        if (BLTINDEX_TRACE) Logger::logDebug( _thread, "", __LOC__ );
        uassert( -1, "NULL == inputKey", NULL != inputKey );

        uchar value[IdLength];
        BLTVal::putPageNo( value, pageNo );
        return insertKeyVal( inputKey, inputKeyLen, value, IdLength, level );
    }

    /**
    *  cache page of keys into cursor and return starting slot for given key
    */
    uint BLTree::startKey( const uchar* key, uint keyLen ) {
        if (BLTINDEX_TRACE) Logger::logDebug( _thread, "", __LOC__ );
        uassert( -1, "NULL == key", NULL != key );

        PageSet set[1];
    
        // cache page for retrieval
        uint slot = _mgr->loadPage( set, key, keyLen, 0, LockRead, _thread );
        if (slot) {
            memcpy( _cursor, set->_page, _mgr->getPageSize() );
        }
        else {
            return 0;
        }
    
        _cursorPage = set->_pageNo;
    
        _mgr->unlockPage( LockRead, set->_latch, _thread );
        _mgr->unpinLatch( set->_latch, _thread );
        _mgr->unpinPoolEntry( set->_pool, _thread );
        return slot;
    }
    
    /**
    *  return next slot for cursor page
    *  or slide cursor right into next page
    */
    uint BLTree::nextKey( uint slot ) {
        if (BLTINDEX_TRACE) Logger::logDebug( _thread, "", __LOC__ );

        PageSet set[1];
    
        do {
            PageNo right = _cursor->_right;
    
            while (slot++ < _cursor->_cnt) {
                if (Page::slotptr(_cursor, slot)->_dead ) {
                    continue;
                }
                else if (right || (slot < _cursor->_cnt) ) { // skip infinite stopper
                    return slot;
                }
                else {
                    break;
                }
            }
    
            if (!right ) break;
            _cursorPage = right;
     
            if ((set->_pool = _mgr->pinPoolEntry( right, _thread ))) {
                set->_page = _mgr->page( set->_pool, right, _thread );
            }
            else {
                return 0;
            }
    
            set->_latch = _mgr->pinLatch( right, _thread );
            _mgr->lockPage( LockRead, set->_latch, _thread );
     
            memcpy( _cursor, set->_page, _mgr->getPageSize() );
     
            _mgr->unlockPage( LockRead, set->_latch, _thread );
            _mgr->unpinLatch( set->_latch, _thread );
            _mgr->unpinPoolEntry( set->_pool, _thread );
            slot = 0;
    
        } while (true);
    
        return (_err = BLTERR_ok);
    }
    
    /**
    *
    */
    void BLTree::latchAudit() {
        if (BLTINDEX_TRACE) Logger::logDebug( _thread, "", __LOC__ );

        //posix_fadvise( _mgr->getFD(), 0, 0, POSIX_FADV_SEQUENTIAL );

        if (*(ushort *)(_mgr->getLatchMgr()->_lock)) {
            Logger::logDebug( _thread, "Alloc page locked", __LOC__ );
        }
        *(ushort *)(_mgr->getLatchMgr()->_lock) = 0;
    
        for (ushort idx = 1; idx <= _mgr->getLatchMgr()->_latchDeployed; idx++ ) {

            LatchSet* latchSet = &_mgr->getLatchMgr()->_latchSets[ idx ];
            PageNo pageNo = latchSet->_pageNo;

            if (*latchSet->_readwr->_rin & MASK) {
                __OSS__( "latchset " << idx << " rw locked for page " << pageNo );
                Logger::logDebug( _thread, __ss__, __LOC__ );
            }
            memset( (ushort *)latchSet->_readwr, 0, sizeof( RWLock ) );
    
            if (*latchSet->_access->_rin & MASK) {
                __OSS__( "latchset " << idx << " access locked for page " << pageNo );
                Logger::logDebug( _thread, __ss__, __LOC__ );
            }
            memset( (ushort *)latchSet->_access, 0, sizeof( RWLock ) );
    
            if (*latchSet->_parent->_rin & MASK) {
                __OSS__( "latchset " << idx << " parent locked for page " << pageNo );
                Logger::logDebug( _thread, __ss__, __LOC__ );
            }
            memset( (ushort *)latchSet->_parent, 0, sizeof( RWLock ) );
    
            if (latchSet->_pin ) {
                __OSS__( "latchset " << idx << " pinned for page " << pageNo );
                Logger::logDebug( _thread, __ss__, __LOC__ );
                latchSet->_pin = 0;
            }
        }
    
        for (ushort hashidx = 0; hashidx < _mgr->getLatchMgr()->_latchHashSize; hashidx++ ) {
            if (*(ushort *)(_mgr->getLatchMgr()->_table[hashidx]._latch) ) {
                __OSS__( "hash entry " << hashidx << " locked" );
                Logger::logDebug( _thread, __ss__, __LOC__ );
            }
            *(ushort *)(_mgr->getLatchMgr()->_table[hashidx]._latch) = 0;
    
            uint idx = _mgr->getLatchMgr()->_table[hashidx]._slot;
            if (idx) {
                LatchSet* latchSet;
                do {
                    latchSet = &_mgr->getLatchMgr()->_latchSets[ idx ];
                    PageNo pageNo = latchSet->_pageNo;

                    if (*(ushort *)latchSet->_busy ) {
                        __OSS__( "latchset " << idx << " busy locked for page " << pageNo );
                        Logger::logDebug( _thread, __ss__, __LOC__ );
                    }
                    *(ushort *)latchSet->_busy = 0;
                    if (latchSet->_hash != hashidx ) {
                        __OSS__( "latchset " << idx << " wrong hashidx " );
                        Logger::logDebug( _thread, __ss__, __LOC__ );
                    }
                    if (latchSet->_pin ) {
                        __OSS__( "latchset " << idx << " pinned for page " << pageNo );
                        Logger::logDebug( _thread, __ss__, __LOC__ );
                    }
                } while ((idx = latchSet->_next));
            }
        }
    
        PageNo next   = _mgr->getLatchMgr()->_nlatchPage + LATCH_page;
        PageNo pageNo = LEAF_page;
        Page* _frame  = (Page *)malloc( _mgr->getPageSize() );
    
        while (pageNo < _mgr->getLatchMgr()->_alloc->_right) {
            pread( _mgr->getFD(), _frame, _mgr->getPageSize(), pageNo << _mgr->getPageBits() );
            if (!_frame->_free) {
                for (uint idx = 0; idx++ < _frame->_cnt - 1; ) {
                    BLTKey* key = Page::keyptr(_frame, idx+1);
                    if (BLTKey::keycmp( Page::keyptr(_frame, idx), key->_key, key->_len ) >= 0) {
                        __OSS__( "page " << pageNo << " idx" << idx << " out of order" );
                        Logger::logDebug( _thread, __ss__, __LOC__ );
                    }
                }
            }
    
            if (pageNo > LEAF_page) next = pageNo + 1;
            pageNo = next;
        }
    }

}   // namespace mongo
