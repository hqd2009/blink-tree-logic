//@file key.h
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

#pragma once

#include "common.h"
#include <string>
#include <string.h>
    
namespace mongo {

    /**
    *  The key structure occupies space at the upper end of each
    *  page.  It's a length byte followed by the value bytes.
    */
    class BLTKey {
    public:

        /**
        *  compare two keys,
        */
        static int keycmp( BLTKey* key1, const uchar* key2, uint len2 ) {
            uint len1 = key1->_len;
            int ans = memcmp(key1->_key, key2, len1 > len2 ? len2 : len1);
            return (ans ? ans : (len1>len2) ? 1 : (len1<len2) ? -1 : 0);
        }

        /**
        *  compare two keys,
        */
        static int keycmp(BLTKey* key1, BLTKey* key2) {
            uint len1 = key1->_len;
            uint len2 = key2->_len;
            int ans = memcmp(key1->_key, key2->_key, len1 > len2 ? len2 : len1);
            return (ans ? ans : (len1>len2) ? 1 : (len1<len2) ? -1 : 0);
        }

        std::string toString() const {
            char buf[ _len + 1 ];
            strncpy( buf, (const char *)_key, _len );
            buf[ _len ] = 0;
            return std::string( buf );
        }

        // data
        uchar _len;
        uchar _key[0];

    };

}   // namespace mongo
