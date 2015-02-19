#include "Index.h"
#include <unistd.h>
#include <zlib.h>
#include <stdio.h>
#include <iostream>
#include <fcntl.h>
#include <map>
#include "kseq.h"
#include "MurmurHash3.h"
#include <assert.h>

#define SET_BINARY_MODE(file)
#define CHUNK 16384
KSEQ_INIT(gzFile, gzread)

using namespace std;

typedef map < Index::hash_t, vector<Index::Locus> > LociByHash_map;

int Index::initFromCapnp(const char * file)
{
	// use a pipe to decompress input to Cap'n Proto
	
	int fds[2];
	int piped = pipe(fds);
	
	if ( piped < 0 )
	{
		cerr << "ERROR: could not open pipe for decompression\n";
		return 1;
	}
	
	int forked = fork();
	
	if ( forked < 0 )
	{
		cerr << "ERROR: could not fork for decompression" << endl;
		return 1;
	}
	
	if ( forked == 0 )
	{
		// read from zipped fd and write to pipe
		
		close(fds[0]); // other process's end of pipe
		
		int fd = open(file, O_RDONLY);
		
		if ( fd < 0 )
		{
			cerr << "ERROR: could not open " << file << " for reading." << endl;
			exit(1);
		}
		
		char buffer[1024];
		
		read(fd, buffer, capnpHeaderLength);
		buffer[capnpHeaderLength] = 0;
		
		if ( strcmp(buffer, capnpHeader) != 0 )
		{
			cerr << "ERROR: '" << file << "' does not look like an index" << endl;
			exit(1);
		}
		
		int ret = inf(fd, fds[1]);
		if (ret != Z_OK) zerr(ret);
		close(fd);
		exit(ret);
		
		gzFile fileIn = gzopen(file, "rb");
		
		int bytesRead;
		
		// eat header
		//
		gzread(fileIn, buffer, capnpHeaderLength);
		
		printf("header: %s\n", buffer);
		while ( (bytesRead = gzread(fileIn, buffer, sizeof(buffer))) > 0)
		{
			printf("uncompressed: %s\n", buffer);
			write(fds[1], buffer, bytesRead);
		}
		
		gzclose(fileIn);
		close(fds[1]);
		exit(0);
	}
	
	// read from pipe
	
	close(fds[1]); // other process's end of pipe
	
	capnp::ReaderOptions readerOptions;
	
	readerOptions.traversalLimitInWords = 1000000000000;
	readerOptions.nestingLimit = 1000000;
	
	capnp::StreamFdMessageReader message(fds[0], readerOptions);
	capnp::MinHash::Reader reader = message.getRoot<capnp::MinHash>();
	
	capnp::MinHash::ReferenceList::Reader referenceListReader = reader.getReferenceList();
	
	capnp::List<capnp::MinHash::ReferenceList::Reference>::Reader referencesReader = referenceListReader.getReferences();
	references.resize(referencesReader.size());
	
	for ( int i = 0; i < references.size(); i++ )
	{
		capnp::MinHash::ReferenceList::Reference::Reader referenceReader = referencesReader[i];
		
		references[i].name = referenceReader.getName();
		references[i].comment = referenceReader.getComment();
		references[i].length = referenceReader.getLength();
	}
	
	capnp::MinHash::HashTable::Reader hashTableReader = reader.getHashTable();
	capnp::List<capnp::MinHash::HashTable::HashBin>::Reader hashBinsReader = hashTableReader.getHashBins();
	
	for ( int i = 0; i < hashBinsReader.size(); i++ )
	{
		capnp::MinHash::HashTable::HashBin::Reader hashBinReader = hashBinsReader[i];
		
		vector<Locus> & loci = lociByHash[hashBinReader.getHash()];
		
		capnp::List<capnp::MinHash::HashTable::HashBin::Locus>::Reader lociReader = hashBinReader.getLoci();
		
		for ( int j = 0; j < lociReader.size(); j++ )
		{
			loci.push_back(Locus(lociReader[j].getSequence(), lociReader[j].getPosition()));
		}
	}
	
	printf("\nCombined hash table:\n\n");
	
	for ( LociByHash_umap::iterator i = lociByHash.begin(); i != lociByHash.end(); i++ )
	{
		printf("Hash %u:\n", i->first);
		
		for ( int j = 0; j < i->second.size(); j++ )
		{
			printf("   Seq: %d\tPos: %d\n", i->second.at(j).sequence, i->second.at(j).position);
		}
	}
	
	close(fds[0]);
	
	return 0;
}

int Index::initFromSequence(const char * file, int kmer, int mins, int stride)
{
	gzFile fp;
	kseq_t *seq;
	int l;
	
	fp = gzopen(file, "r");
	seq = kseq_init(fp);
	int count = 0;
	
	while ((l = kseq_read(seq)) >= 0)
	{
		LociByHash_map lociByHashLocal;
		
		printf("name: %s\n", seq->name.s);
		if (seq->comment.l) printf("comment: %s\n", seq->comment.s);
		printf("seq: %s\n", seq->seq.s);
		if (seq->qual.l) printf("qual: %s\n", seq->qual.s);
		
		references.resize(references.size() + 1);
		
		references[references.size() - 1].name = seq->name.s;
		
		if ( seq->comment.l > 0 )
		{
			references[references.size() - 1].comment = seq->comment.s;
		}
		
		references[references.size() - 1].length = l;
		
		for ( int i = 0; i < seq->seq.l - kmer + 1; i += stride )
		{
			hash_t hash;
			MurmurHash3_x86_32(seq->seq.s + i, kmer, seed, &hash);
			printf("   Hash at pos %d:\t%u\n", i, hash);
			
			if
			(
				lociByHashLocal.count(hash) == 0 &&
				(
					lociByHashLocal.size() < mins ||
					hash < lociByHashLocal.rbegin()->first
				)
			)
			{
				lociByHashLocal.insert(pair<hash_t, vector<Locus> >(hash, vector<Locus>())); // insert empty vector
				
				if ( lociByHashLocal.size() > mins )
				{
					lociByHashLocal.erase(--lociByHashLocal.end());
				}
			}
			
			if ( lociByHashLocal.count(hash) )
			{
				lociByHashLocal[hash].push_back(Locus(count, i));
			}
		}
		
		printf("\n");
		
		for ( LociByHash_map::iterator i = lociByHashLocal.begin(); i != lociByHashLocal.end(); i++ )
		{
			printf("Hash %u:\n", i->first);
		
			for ( int j = 0; j < i->second.size(); j++ )
			{
				printf("   Seq: %d\tPos: %d\n", i->second.at(j).sequence, i->second.at(j).position);
			}
			
			const vector<Locus> & lociLocal = i->second;
			vector<Locus> & lociGlobal = lociByHash[i->first]; // creates if needed
			
			lociGlobal.insert(lociGlobal.end(), lociLocal.begin(), lociLocal.end());
		}
		
		count++;
	}
	
	if ( l != -1 ) printf("ERROR: return value: %d\n", l);
	kseq_destroy(seq);
	gzclose(fp);
	
	printf("\nCombined hash table:\n\n");
	
	for ( LociByHash_umap::iterator i = lociByHash.begin(); i != lociByHash.end(); i++ )
	{
		printf("Hash %u:\n", i->first);
		
		for ( int j = 0; j < i->second.size(); j++ )
		{
			printf("   Seq: %d\tPos: %d\n", i->second.at(j).sequence, i->second.at(j).position);
		}
	}
	
	return 0;
}

int Index::writeToCapnp(const char * file) const
{
	// use a pipe to compress Cap'n Proto output
	
	int fds[2];
	int piped = pipe(fds);
	
	if ( piped < 0 )
	{
		cerr << "ERROR: could not open pipe for compression\n";
		return 1;
	}
	
	int forked = fork();
	
	if ( forked < 0 )
	{
		cerr << "ERROR: could not fork for compression\n";
		return 1;
	}
	
	if ( forked == 0 )
	{
		// read from pipe and write to compressed file
		
		close(fds[1]); // other process's end of pipe
		
		int fd = open(file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
		
		if ( fd < 0 )
		{
			cerr << "ERROR: could not open " << file << " for writing.\n";
			exit(1);
		}
		
		// write header
		//
		write(fd, capnpHeader, capnpHeaderLength);
		
		int ret = def(fds[0], fd, Z_DEFAULT_COMPRESSION);
		if (ret != Z_OK) zerr(ret);
        exit(ret);
        
		char buffer[1024];
		gzFile fileOut = gzopen(file, "ab");
		
		int bytesRead;
		
		while ( (bytesRead = read(fds[0], buffer, sizeof(buffer))) > 0)
		{
			printf("compressing: %s\n", buffer);
			gzwrite(fileOut, buffer, bytesRead);
		}
		
		gzclose(fileOut);
		close(fds[0]);
		exit(0);
	}
	
	// write to pipe
	
	close(fds[0]); // other process's end of pipe
	
	capnp::MallocMessageBuilder message;
	capnp::MinHash::Builder builder = message.initRoot<capnp::MinHash>();
	
	capnp::MinHash::ReferenceList::Builder referenceListBuilder = builder.initReferenceList();
	
	capnp::List<capnp::MinHash::ReferenceList::Reference>::Builder referencesBuilder = referenceListBuilder.initReferences(references.size());
	
	for ( int i = 0; i < references.size(); i++ )
	{
		capnp::MinHash::ReferenceList::Reference::Builder referenceBuilder = referencesBuilder[i];
		
		referenceBuilder.setName(references[i].name);
		referenceBuilder.setComment(references[i].comment);
		referenceBuilder.setLength(references[i].length);
	}
	
	capnp::MinHash::HashTable::Builder hashTableBuilder = builder.initHashTable();
	capnp::List<capnp::MinHash::HashTable::HashBin>::Builder hashBinsBuilder = hashTableBuilder.initHashBins(lociByHash.size());
	
	int hashIndex = 0;
	
	for ( LociByHash_umap::const_iterator i = lociByHash.begin(); i != lociByHash.end(); i++ )
	{
		capnp::MinHash::HashTable::HashBin::Builder hashBinBuilder = hashBinsBuilder[hashIndex];
		hashIndex++;
		
		hashBinBuilder.setHash(i->first);
		
		const vector<Locus> & loci = i->second;
		
		capnp::List<capnp::MinHash::HashTable::HashBin::Locus>::Builder lociBuilder = hashBinBuilder.initLoci(loci.size());
		
		for ( int j = 0; j < loci.size(); j++ )
		{
			lociBuilder[j].setSequence(loci[j].sequence);
			lociBuilder[j].setPosition(loci[j].position);
		}
	}
	
	writeMessageToFd(fds[1], message);
	close(fds[1]);
	
	return 0;
}


// The following functions are adapted from http://www.zlib.net/zpipe.c


/* Compress from file source to file dest until EOF on source.
   def() returns Z_OK on success, Z_MEM_ERROR if memory could not be
   allocated for processing, Z_STREAM_ERROR if an invalid compression
   level is supplied, Z_VERSION_ERROR if the version of zlib.h and the
   version of the library linked do not match, or Z_ERRNO if there is
   an error reading or writing the files. */
int def(int fdSource, int fdDest, int level)
{
    int ret, flush;
    unsigned have;
    z_stream strm;
    unsigned char in[CHUNK];
    unsigned char out[CHUNK];

    /* allocate deflate state */
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    ret = deflateInit(&strm, level);
    if (ret != Z_OK)
        return ret;

    /* compress until end of file */
    do {
        strm.avail_in = read(fdSource, in, CHUNK);
        if (strm.avail_in == -1) {
            (void)deflateEnd(&strm);
            return Z_ERRNO;
        }
        flush = strm.avail_in == 0 ? Z_FINISH : Z_NO_FLUSH;
        strm.next_in = in;

        /* run deflate() on input until output buffer not full, finish
           compression if all of source has been read in */
        do {
            strm.avail_out = CHUNK;
            strm.next_out = out;
            ret = deflate(&strm, flush);    /* no bad return value */
            assert(ret != Z_STREAM_ERROR);  /* state not clobbered */
            have = CHUNK - strm.avail_out;
            if (write(fdDest, out, have) != have) {
                (void)deflateEnd(&strm);
                return Z_ERRNO;
            }
        } while (strm.avail_out == 0);
        assert(strm.avail_in == 0);     /* all input will be used */

        /* done when last data in file processed */
    } while (flush != Z_FINISH);
    assert(ret == Z_STREAM_END);        /* stream will be complete */

    /* clean up and return */
    (void)deflateEnd(&strm);
    return Z_OK;
}

/* Decompress from file source to file dest until stream ends or EOF.
   inf() returns Z_OK on success, Z_MEM_ERROR if memory could not be
   allocated for processing, Z_DATA_ERROR if the deflate data is
   invalid or incomplete, Z_VERSION_ERROR if the version of zlib.h and
   the version of the library linked do not match, or Z_ERRNO if there
   is an error reading or writing the files. */
int inf(int fdSource, int fdDest)
{
    int ret;
    unsigned have;
    z_stream strm;
    unsigned char in[CHUNK];
    unsigned char out[CHUNK];

    /* allocate inflate state */
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit(&strm);
    if (ret != Z_OK)
        return ret;

    /* decompress until deflate stream ends or end of file */
    do {
        strm.avail_in = read(fdSource, in, CHUNK);
        if (strm.avail_in == -1) {
            (void)inflateEnd(&strm);
            return Z_ERRNO;
        }
        if (strm.avail_in == 0)
            break;
        strm.next_in = in;

        /* run inflate() on input until output buffer not full */
        do {
            strm.avail_out = CHUNK;
            strm.next_out = out;
            ret = inflate(&strm, Z_NO_FLUSH);
            assert(ret != Z_STREAM_ERROR);  /* state not clobbered */
            switch (ret) {
            case Z_NEED_DICT:
                ret = Z_DATA_ERROR;     /* and fall through */
            case Z_DATA_ERROR:
            case Z_MEM_ERROR:
                (void)inflateEnd(&strm);
                return ret;
            }
            have = CHUNK - strm.avail_out;
            if (write(fdDest, out, have) != have) {
                (void)inflateEnd(&strm);
                return Z_ERRNO;
            }
        } while (strm.avail_out == 0);

        /* done when inflate() says it's done */
    } while (ret != Z_STREAM_END);

    /* clean up and return */
    (void)inflateEnd(&strm);
    return ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
}

/* report a zlib or i/o error */
void zerr(int ret)
{
    fputs("zpipe: ", stderr);
    switch (ret) {
    case Z_ERRNO:
        if (ferror(stdin))
            fputs("error reading stdin\n", stderr);
        if (ferror(stdout))
            fputs("error writing stdout\n", stderr);
        break;
    case Z_STREAM_ERROR:
        fputs("invalid compression level\n", stderr);
        break;
    case Z_DATA_ERROR:
        fputs("invalid or incomplete deflate data\n", stderr);
        break;
    case Z_MEM_ERROR:
        fputs("out of memory\n", stderr);
        break;
    case Z_VERSION_ERROR:
        fputs("zlib version mismatch!\n", stderr);
    }
}