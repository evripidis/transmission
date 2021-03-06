/*
 * This file Copyright (C) 2009-2010 Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2.  Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id$
 */

#include <assert.h>
#include <stdio.h> /* remove() */

#include <event2/buffer.h>

#include "transmission.h"
#include "bencode.h"
#include "crypto.h"
#include "magnet.h"
#include "metainfo.h"
#include "resume.h"
#include "torrent.h"
#include "torrent-magnet.h"
#include "utils.h"
#include "web.h"

#define dbgmsg( tor, ... ) \
    do { \
        if( tr_deepLoggingIsActive( ) ) \
            tr_deepLog( __FILE__, __LINE__, tor->info.name, __VA_ARGS__ ); \
    } while( 0 )

/***
****
***/

enum
{
    /* don't ask for the same metadata piece more than this often */
    MIN_REPEAT_INTERVAL_SECS = 3
};

struct metadata_node
{
    time_t requestedAt;
    int piece;
};

struct tr_incomplete_metadata
{
    uint8_t * metadata;
    int metadata_size;
    int pieceCount;

    /** sorted from least to most recently requested */
    struct metadata_node * piecesNeeded;
    int piecesNeededCount;
};

static void
incompleteMetadataFree( struct tr_incomplete_metadata * m )
{
    tr_free( m->metadata );
    tr_free( m->piecesNeeded );
    tr_free( m );
}

void
tr_torrentSetMetadataSizeHint( tr_torrent * tor, int size )
{
    if( !tr_torrentHasMetadata( tor ) )
    {
        if( tor->incompleteMetadata == NULL )
        {
            int i;
            struct tr_incomplete_metadata * m;
            const int n = ( size + ( METADATA_PIECE_SIZE - 1 ) ) / METADATA_PIECE_SIZE;
            dbgmsg( tor, "metadata is %d bytes in %d pieces", size, n );

            m = tr_new( struct tr_incomplete_metadata, 1 );
            m->pieceCount = n;
            m->metadata = tr_new( uint8_t, size );
            m->metadata_size = size;
            m->piecesNeededCount = n;
            m->piecesNeeded = tr_new( struct metadata_node, n );

            for( i=0; i<n; ++i ) {
                m->piecesNeeded[i].piece = i;
                m->piecesNeeded[i].requestedAt = 0;
            }

            tor->incompleteMetadata = m;
        }
    }
}

static int
findInfoDictOffset( const tr_torrent * tor )
{
    size_t fileLen;
    uint8_t * fileContents;
    int offset = 0;

    /* load the file, and find the info dict's offset inside the file */
    if(( fileContents = tr_loadFile( tor->info.torrent, &fileLen )))
    {
        tr_benc top;

        if( !tr_bencParse( fileContents, fileContents + fileLen, &top, NULL ) )
        {
            tr_benc * infoDict;

            if( tr_bencDictFindDict( &top, "info", &infoDict ) )
            {
                int infoLen;
                char * infoContents = tr_bencToStr( infoDict, TR_FMT_BENC, &infoLen );
                const uint8_t * i = (const uint8_t*) tr_memmem( (char*)fileContents, fileLen, infoContents, infoLen );
                offset = i != NULL ? i - fileContents : 0;
                tr_free( infoContents );
            }

            tr_bencFree( &top );
        }

        tr_free( fileContents );
    }

    return offset;
}

static void
ensureInfoDictOffsetIsCached( tr_torrent * tor )
{
    assert( tr_torrentHasMetadata( tor ) );

    if( !tor->infoDictOffsetIsCached )
    {
        tor->infoDictOffset = findInfoDictOffset( tor );
        tor->infoDictOffsetIsCached = TRUE;
    }
}

void*
tr_torrentGetMetadataPiece( tr_torrent * tor, int piece, int * len )
{
    char * ret = NULL;

    assert( tr_isTorrent( tor ) );
    assert( piece >= 0 );
    assert( len != NULL );

    if( tr_torrentHasMetadata( tor ) )
    {
        FILE * fp;

        ensureInfoDictOffsetIsCached( tor );

        assert( tor->infoDictLength > 0 );
        assert( tor->infoDictOffset >= 0 );

        fp = fopen( tor->info.torrent, "rb" );
        if( fp != NULL )
        {
            const int o = piece  * METADATA_PIECE_SIZE;

            if( !fseek( fp, tor->infoDictOffset + o, SEEK_SET ) )
            {
                const int l = o + METADATA_PIECE_SIZE <= tor->infoDictLength
                            ? METADATA_PIECE_SIZE
                            : tor->infoDictLength - o;

                if( 0<l && l<=METADATA_PIECE_SIZE )
                {
                    char * buf = tr_new( char, l );
                    const int n = fread( buf, 1, l, fp );
                    if( n == l )
                    {
                        *len = l;
                        ret = buf;
                        buf = NULL;
                    }

                    tr_free( buf );
                }
            }

            fclose( fp );
        }
    }

    return ret;
}

void
tr_torrentSetMetadataPiece( tr_torrent  * tor, int piece, const void  * data, int len )
{
    int i;
    struct tr_incomplete_metadata * m;
    const int offset = piece * METADATA_PIECE_SIZE;

    assert( tr_isTorrent( tor ) );

    dbgmsg( tor, "got metadata piece %d", piece );

    /* are we set up to download metadata? */
    m = tor->incompleteMetadata;
    if( m == NULL )
        return;

    /* does this data pass the smell test? */
    if( offset + len > m->metadata_size )
        return;

    /* do we need this piece? */
    for( i=0; i<m->piecesNeededCount; ++i )
        if( m->piecesNeeded[i].piece == piece )
            break;
    if( i==m->piecesNeededCount )
        return;

    memcpy( m->metadata + offset, data, len );

    tr_removeElementFromArray( m->piecesNeeded, i,
                               sizeof( struct metadata_node ),
                               m->piecesNeededCount-- );

    dbgmsg( tor, "saving metainfo piece %d... %d remain", piece, m->piecesNeededCount );

    /* are we done? */
    if( m->piecesNeededCount == 0 )
    {
        tr_bool success = FALSE;
        tr_bool checksumPassed = FALSE;
        tr_bool metainfoParsed = FALSE;
        uint8_t sha1[SHA_DIGEST_LENGTH];

        /* we've got a complete set of metainfo... see if it passes the checksum test */
        dbgmsg( tor, "metainfo piece %d was the last one", piece );
        tr_sha1( sha1, m->metadata, m->metadata_size, NULL );
        if(( checksumPassed = !memcmp( sha1, tor->info.hash, SHA_DIGEST_LENGTH )))
        {
            /* checksum passed; now try to parse it as benc */
            tr_benc infoDict;
            const int err = tr_bencLoad( m->metadata, m->metadata_size, &infoDict, NULL );
            dbgmsg( tor, "err is %d", err );
            if(( metainfoParsed = !err ))
            {
                /* yay we have bencoded metainfo... merge it into our .torrent file */
                tr_benc newMetainfo;
                char * path = tr_strdup( tor->info.torrent );

                if( !tr_bencLoadFile( &newMetainfo, TR_FMT_BENC, path ) )
                {
                    tr_bool hasInfo;
                    tr_info info;
                    int infoDictLength;

                    /* remove any old .torrent and .resume files */
                    remove( path );
                    tr_torrentRemoveResume( tor );

                    dbgmsg( tor, "Saving completed metadata to \"%s\"", path );
                    tr_bencMergeDicts( tr_bencDictAddDict( &newMetainfo, "info", 0 ), &infoDict );

                    memset( &info, 0, sizeof( tr_info ) );
                    success = tr_metainfoParse( tor->session, &newMetainfo, &info, &hasInfo, &infoDictLength );

                    if( success && !tr_getBlockSize( info.pieceSize ) )
                    {
                        tr_torrentSetLocalError( tor, "%s", _( "Magnet torrent's metadata is not usable" ) );
                        success = FALSE;
                    }

                    if( success )
                    {
                        /* keep the new info */
                        tor->info = info;
                        tor->infoDictLength = infoDictLength;

                        /* save the new .torrent file */
                        tr_bencToFile( &newMetainfo, TR_FMT_BENC, tor->info.torrent );
                        tr_sessionSetTorrentFile( tor->session, tor->info.hashString, tor->info.torrent );
                        tr_torrentGotNewInfoDict( tor );
                        tr_torrentSetDirty( tor );
                    }

                    tr_bencFree( &newMetainfo );
                }

                tr_bencFree( &infoDict );
                tr_free( path );
            }
        }

        if( success )
        {
            incompleteMetadataFree( tor->incompleteMetadata );
            tor->incompleteMetadata = NULL;
        }
        else /* drat. */
        {
            const int n = m->pieceCount;
            for( i=0; i<n; ++i )
            {
                m->piecesNeeded[i].piece = i;
                m->piecesNeeded[i].requestedAt = 0;
            }
            m->piecesNeededCount = n;
            dbgmsg( tor, "metadata error; trying again. %d pieces left", n );

            tr_err( "magnet status: checksum passed %d, metainfo parsed %d",
                    (int)checksumPassed, (int)metainfoParsed );
        }
    }
}

tr_bool
tr_torrentGetNextMetadataRequest( tr_torrent * tor, time_t now, int * setme_piece )
{
    tr_bool have_request = FALSE;
    struct tr_incomplete_metadata * m;

    assert( tr_isTorrent( tor ) );

    m = tor->incompleteMetadata;

    if( ( m != NULL )
        && ( m->piecesNeededCount > 0 )
        && ( m->piecesNeeded[0].requestedAt + MIN_REPEAT_INTERVAL_SECS < now ) )
    {
        int i;
        const int piece = m->piecesNeeded[0].piece;

        tr_removeElementFromArray( m->piecesNeeded, 0,
                                   sizeof( struct metadata_node ),
                                   m->piecesNeededCount-- );

        i = m->piecesNeededCount++;
        m->piecesNeeded[i].piece = piece;
        m->piecesNeeded[i].requestedAt = now;

        dbgmsg( tor, "next piece to request: %d", piece );
        *setme_piece = piece;
        have_request = TRUE;
    }

    return have_request;
}

double
tr_torrentGetMetadataPercent( const tr_torrent * tor )
{
    double ret;

    if( tr_torrentHasMetadata( tor ) )
        ret = 1.0;
    else {
        const struct tr_incomplete_metadata * m = tor->incompleteMetadata;
        if( m == NULL )
            ret = 0.0;
        else
            ret = (m->pieceCount - m->piecesNeededCount) / (double)m->pieceCount;
    }

    return ret;
}

char*
tr_torrentGetMagnetLink( const tr_torrent * tor )
{
    int i;
    const char * name;
    struct evbuffer * s;

    assert( tr_isTorrent( tor ) );

    s = evbuffer_new( );
    evbuffer_add_printf( s, "magnet:?xt=urn:btih:%s", tor->info.hashString );
    name = tr_torrentName( tor );
    if( name && *name )
    {
        evbuffer_add_printf( s, "%s", "&dn=" );
        tr_http_escape( s, tr_torrentName( tor ), -1, TRUE );
    }
    for( i=0; i<tor->info.trackerCount; ++i )
    {
        evbuffer_add_printf( s, "%s", "&tr=" );
        tr_http_escape( s, tor->info.trackers[i].announce, -1, TRUE );
    }

    return evbuffer_free_to_str( s );
}
