/**
 * @file      FASTA.cpp
 * @brief     Compression/Decompression of FASTA
 * @author    Morteza Hosseini  (seyedmorteza@ua.pt)
 * @author    Diogo Pratas      (pratas@ua.pt)
 * @author    Armando J. Pinho  (ap@ua.pt)
 * @copyright The GNU General Public License v3.0
 */

#include <fstream>
#include <thread>
#include <mutex>
#include <chrono>       // time
#include <iomanip>      // setw, setprecision
#include <cstring>
#include "FASTA.h"

//#include <functional>
//#include <algorithm>

using std::chrono::high_resolution_clock;
using std::thread;
using std::cout;
using std::cerr;
using std::ifstream;
using std::ofstream;
using std::to_string;
using std::setprecision;
using std::memset;

std::mutex mutxFA;    /**< @brief Mutex */


/**
 * @brief Compress
 */
void FASTA::compress ()
{
    // Start timer for compression
    high_resolution_clock::time_point startTime = high_resolution_clock::now();

    thread   arrThread[N_THREADS];
    byte     t;           // For threads
    string   headers;
    packfa_s pkStruct;    // Collection of inputs to pass to pack...

    if (VERBOSE)   cerr << "Calculating number of different characters...\n";
    // Gather different chars in all headers and max length in all bases
    gatherHdrBs(headers);
    // Show number of different chars in headers -- ignore '>'=62
    if (VERBOSE)   cerr << "In headers, they are " << headers.length() << ".\n";
    
    // Set Hash table and pack function
    set_hashTbl_packFn(pkStruct, headers);
    
    // Distribute file among threads, for reading and packing
    for (t = 0; t != N_THREADS; ++t)
        arrThread[t] = thread(&FASTA::pack, this, pkStruct, t);
    for (t = 0; t != N_THREADS; ++t)
        if (arrThread[t].joinable())    arrThread[t].join();
    
    if (VERBOSE)    cerr << "Shuffling done!\n";
    
    // Join partially packed files
    joinPackedFiles(headers);
    
    // Stop timer for compression
    high_resolution_clock::time_point finishTime = high_resolution_clock::now();
    // Compression duration in seconds
    std::chrono::duration<double> elapsed = finishTime - startTime;
    
    cerr << (VERBOSE ? "Compaction done" : "Done") << ", in "
         << std::fixed << setprecision(4) << elapsed.count() << " seconds.\n";
    
    // Cout encrypted content
    encrypt();
}

/**
 * @brief      Set hash table and pack function
 * @param[out] pkStruct  Pack structure
 * @param[in]  headers   Headers
 */
void FASTA::set_hashTbl_packFn (packfa_s &pkStruct, const string &headers)
{
    const size_t headersLen = headers.length();
    packFP_t packHdr;    // Function pointer
    
    // Header
    if (headersLen > MAX_C5)             // If len > 39, filter the last 39 ones
    {
        Hdrs = headers.substr(headersLen - MAX_C5);
        // ASCII char after the last char in Hdrs -- always <= (char) 127
        HdrsX = Hdrs;    HdrsX += (char) (Hdrs.back() + 1);
        buildHashTbl(HdrMap, HdrsX, KEYLEN_C5);
        packHdr = &EnDecrypto::packLHdrFaFq;
    }
    else
    {
        Hdrs = headers;
        
        if (headersLen > MAX_C4)                            // 16 <= cat 5 <= 39
        {
            buildHashTbl(HdrMap, Hdrs, KEYLEN_C5);
            packHdr = &EnDecrypto::pack_3to2;
        }
        else if (headersLen > MAX_C3)                       // 7 <= cat 4 <= 15
        {
            buildHashTbl(HdrMap, Hdrs, KEYLEN_C4);
            packHdr = &EnDecrypto::pack_2to1;
        }
        else if (headersLen==MAX_C3 || headersLen==MID_C3   // 4 <= cat 3 <= 6
                 || headersLen==MIN_C3)
        {
            buildHashTbl(HdrMap, Hdrs, KEYLEN_C3);
            packHdr = &EnDecrypto::pack_3to1;
        }
        else if (headersLen == C2)                          // cat 2 = 3
        {
            buildHashTbl(HdrMap, Hdrs, KEYLEN_C2);
            packHdr = &EnDecrypto::pack_5to1;
        }
        else if (headersLen == C1)                          // cat 1 = 2
        {
            buildHashTbl(HdrMap, Hdrs, KEYLEN_C1);
            packHdr = &EnDecrypto::pack_7to1;
        }
        else                                                // headersLen = 1
        {
            buildHashTbl(HdrMap, Hdrs, 1);
            packHdr = &EnDecrypto::pack_1to1;
        }
    }
    
    pkStruct.packHdrFP = packHdr;
}

/**
 * @brief Pack. '>' at the beginning of headers is not packed
 * @param pkStruct  Pack structure
 * @param threadID  Thread ID
 */
void FASTA::pack (const packfa_s &pkStruct, byte threadID)
{
    packFP_t packHdr = pkStruct.packHdrFP;    // Function pointer
    ifstream in(IN_FILE_NAME);
    string   line, context, seq;
    ofstream pkfile(PK_FILENAME+to_string(threadID), std::ios_base::app);

    // Lines ignored at the beginning
    for (u64 l = (u64) threadID*BlockLine; l--;)    IGNORE_THIS_LINE(in);

    while (in.peek() != EOF)
    {
        context.clear();
        seq.clear();

        for (u64 l = BlockLine; l-- && getline(in, line).good();)
        {
            // Header
            if (line[0] == '>')
            {
                // Previous seq
                if (!seq.empty())
                {
                    seq.pop_back();                      // Remove the last '\n'
                    packSeq(context, seq);
                    context += (char) 254;
                }
                seq.clear();

                // Header line
                context += (char) 253;
                (this->*packHdr) (context, line.substr(1), HdrMap);
                context += (char) 254;
            }

            // Empty line. (char) 252 instead of line feed
            else if (line.empty()) { seq += (char) 252; }

            // Sequence
            else
            {
                //todo. check if it's needed to check for blank char
//                if (line.find(' ') != string::npos)
//              { cerr<< "Invalid sequence -- spaces not allowed.\n"; exit(1); }

                // (char) 252 instead of '\n' at the end of each seq line
                seq += line;
                seq += (char) 252;
            }
        }
        if (!seq.empty())
        {
            seq.pop_back();                              // Remove the last '\n'

            // The last seq
            packSeq(context, seq);
            context += (char) 254;
        }
        
        // Shuffle
        if (!DISABLE_SHUFFLE)
        {
            mutxFA.lock();//----------------------------------------------------
            if (VERBOSE && shuffInProgress)    cerr << "Shuffling...\n";
            shuffInProgress = false;
            mutxFA.unlock();//--------------------------------------------------
    
            shuffle(context);
        }

        // For unshuffling: insert the size of packed context in the beginning
        string contextSize;
        contextSize += (char) 253;
        contextSize += to_string(context.size());
        contextSize += (char) 254;
        context.insert(0, contextSize);

        // Write header containing threadID for each partially packed file
        pkfile << THR_ID_HDR << to_string(threadID) << '\n';
        pkfile << context << '\n';

        // Ignore to go to the next related chunk
        for (u64 l = (u64) (N_THREADS-1)*BlockLine; l--;)  IGNORE_THIS_LINE(in);
    }

    pkfile.close();
    in.close();
}

/**
 * @brief Join partially packed files
 * @param headers  Headers
 */
void FASTA::joinPackedFiles (const string &headers)  const
{
    byte     t;                    // For threads
    ifstream pkFile[N_THREADS];    // Join partially packed files
    
    // Watermark for encrypted file
    cout << "#cryfa v" + VERSION_CRYFA + "." + RELEASE_CRYFA + "\n";
    
    // Open packed file
    ofstream pckdFile(PCKD_FILENAME);
    pckdFile << (char) 127;                // Let decryptor know this is FASTA
    pckdFile << (!DISABLE_SHUFFLE ? (char) 128 : (char) 129); //Shuffling on/off
    pckdFile << headers;                   // Send headers to decryptor
    pckdFile << (char) 254;                // To detect headers in decompressor
    
    // Open input files
    for (t = 0; t != N_THREADS; ++t)   pkFile[t].open(PK_FILENAME+to_string(t));
    
    string line;
    bool prevLineNotThrID;                 // If previous line was "THR=" or not
    while (!pkFile[0].eof())
    {
        for (t = 0; t != N_THREADS; ++t)
        {
            prevLineNotThrID = false;
            
            while (getline(pkFile[t], line).good() &&
                   line != THR_ID_HDR+to_string(t))
            {
                if (prevLineNotThrID)   pckdFile << '\n';
                pckdFile << line;
                
                prevLineNotThrID = true;
            }
        }
    }
    pckdFile << (char) 252;
    
    // Close/delete input/output files
    pckdFile.close();
    string pkFileName;
    for (t = 0; t != N_THREADS; ++t)
    {
        pkFile[t].close();
        pkFileName=PK_FILENAME;    pkFileName+=to_string(t);
        std::remove(pkFileName.c_str());
    }
}

/**
 * @brief      Gather chars of all headers & max length of
 *             DNA bases lines, excluding '>'
 * @param[out] headers  Chars of all headers
 */
void FASTA::gatherHdrBs (string &headers)
{
    u32  maxBLen=0;           // Max length of each line of bases
    bool hChars[127];
    memset(hChars+32, false, 95);

    ifstream in(IN_FILE_NAME);
    string   line;
    while (getline(in, line).good())
    {
        if (line[0] == '>')
            for (const char &c : line)    hChars[c] = true;
        else
        if (line.size() > maxBLen)    maxBLen = (u32) line.size();
    }
    in.close();

    // Number of lines read from input file while compression
    BlockLine = (u32) (BLOCK_SIZE / maxBLen);
    if (!BlockLine)   BlockLine = 2;

    // Gather the characters -- Ignore '>'=62 for headers
    for (byte i = 32; i != 62;  ++i)    if (*(hChars+i))  headers += i;
    for (byte i = 63; i != 127; ++i)    if (*(hChars+i))  headers += i;
}

/**
 * @brief Decompress
 */
void FASTA::decompress ()
{
    // Start timer for decompression
    high_resolution_clock::time_point startTime = high_resolution_clock::now();

    char       c;                   // Chars in file
    string     headers;
    unpackfa_s upkStruct;           // Collection of inputs to pass to unpack...
    string     chunkSizeStr;        // Chunk size (string) -- For unshuffling
    thread     arrThread[N_THREADS];// Array of threads
    byte       t;                   // For threads
    u64        offset;              // To traverse decompressed file
    ifstream   in(DEC_FILENAME);
    
    in.ignore(1);                   // Jump over decText[0]==(char) 127
    in.get(c);    shuffled = (c==(char) 128); // Check if file had been shuffled
    while (in.get(c) && c != (char) 254)    headers += c;
    
    if (VERBOSE)   // Show number of different chars in headers -- Ignore '>'=62
        cerr << headers.length() << " different characters are in headers.\n";
    
    unpackFP_t unpackHdr;           // Function pointer
    
    // Header -- Set unpack table and unpack function
    set_unpackTbl_unpackFn(upkStruct, headers);
    
    // Distribute file among threads, for reading and unpacking
    typedef void (FASTA::*unpackHFP_t) (const unpackfa_s&, byte);
    unpackHFP_t unpackH =
            (headers.length() <= MAX_C5) ? &FASTA::unpackHS : &FASTA::unpackHL;
    
    for (t = 0; t != N_THREADS; ++t)
    {
        in.get(c);
        if (c == (char) 253)
        {
            chunkSizeStr.clear();   // Chunk size
            while (in.get(c) && c != (char) 254)    chunkSizeStr += c;
            offset = stoull(chunkSizeStr);
            
            upkStruct.begPos    = in.tellg();
            upkStruct.chunkSize = offset;
    
            arrThread[t] = thread(unpackH, this, upkStruct, t);
            
            // Jump to the beginning of the next chunk
            in.seekg((std::streamoff) offset, std::ios_base::cur);
        }
        // End of file
        if (in.peek() == 252)    break;
    }
    // Join threads
    for (t = 0; t != N_THREADS; ++t)
        if (arrThread[t].joinable())    arrThread[t].join();
    
    if (VERBOSE)    cerr << "Unshuffling done!\n";
    
    // Close/delete decrypted file
    in.close();
    const string decFileName = DEC_FILENAME;
    std::remove(decFileName.c_str());
    
    // Join partially unpacked files
    joinUnpackedFiles();
    
    // Stop timer for decompression
    high_resolution_clock::time_point finishTime = high_resolution_clock::now();
    // Decompression duration in seconds
    std::chrono::duration<double> elapsed = finishTime - startTime;

    cerr << (VERBOSE ? "Decompression done" : "Done") << ", in "
         << std::fixed << setprecision(4) << elapsed.count() << " seconds.\n";
}

/**
 * @brief      Set unpack table and unpack function
 * @param[out] upkStruct  Unpack structure
 * @param[in]  headers    Headers
 */
void FASTA::set_unpackTbl_unpackFn(unpackfa_s &upkStruct, const string &headers)
{
    const size_t headersLen = headers.length();
    u16 keyLen_hdr = 0;
    
    if (headersLen > MAX_C5)                keyLen_hdr = KEYLEN_C5;
    else if (headersLen > MAX_C4)                                       // Cat 5
    {
        upkStruct.unpackHdrFP = &EnDecrypto::unpack_2B;
        keyLen_hdr = KEYLEN_C5;
    }
    else
    {
        upkStruct.unpackHdrFP = &EnDecrypto::unpack_1B;
    
        if (headersLen > MAX_C3)            keyLen_hdr = KEYLEN_C4;     // Cat 4
        else if (headersLen==MAX_C3 || headersLen==MID_C3 || headersLen==MIN_C3)
                                            keyLen_hdr = KEYLEN_C3;     // Cat 3
        else if (headersLen == C2)          keyLen_hdr = KEYLEN_C2;     // Cat 2
        else if (headersLen == C1)          keyLen_hdr = KEYLEN_C1;     // Cat 1
        else                                keyLen_hdr = 1;             // = 1
    }
    
    // Build unpacking tables
    if (headersLen <= MAX_C5)
        buildUnpackTbl(upkStruct.hdrUnpack, headers, keyLen_hdr);
    else
    {
        const string decHeaders = headers.substr(headersLen - MAX_C5);
        // ASCII char after the last char in headers string
        string decHeadersX = decHeaders;
        decHeadersX += (upkStruct.XChar_hdr = (char) (decHeaders.back() + 1));
        
        buildUnpackTbl(upkStruct.hdrUnpack, decHeadersX, keyLen_hdr);
    }
}

/**
 * @brief Unpack: small header
 * @param upkStruct  Unpack structure
 * @param threadID   Thread ID
 */
void FASTA::unpackHS (const unpackfa_s &upkStruct, byte threadID)
{
    unpackFP_t unpackHdr = upkStruct.unpackHdrFP;    // Function pointer
    pos_t      begPos    = upkStruct.begPos;
    u64        chunkSize = upkStruct.chunkSize;
    ifstream   in(DEC_FILENAME);
    string     decText, chunkSizeStr;
    string::iterator i;
    char       c;
    pos_t      endPos;
    ofstream   upkfile(UPK_FILENAME+to_string(threadID), std::ios_base::app);
    string     upkhdrOut, upkSeqOut;
    
    while (in.peek() != EOF)
    {
        in.seekg(begPos);      // Read the file from this position
        // Take a chunk of decrypted file
        decText.clear();
        for (u64 u = chunkSize; u--;) { in.get(c);    decText += c; }
        i = decText.begin();
        endPos = in.tellg();   // Set the end position

        // Unshuffle
        if (shuffled)
        {
            mutxFA.lock();//----------------------------------------------------
            if (VERBOSE && shuffInProgress)    cerr << "Unshuffling...\n";
            shuffInProgress = false;
            mutxFA.unlock();//--------------------------------------------------
    
            unshuffle(i, chunkSize);
        }

        upkfile << THR_ID_HDR + to_string(threadID) << '\n';
        do {
            if (*i == (char) 253)                                         // Hdr
            {
                (this->*unpackHdr) (upkhdrOut, ++i, upkStruct.hdrUnpack);
                upkfile << '>' << upkhdrOut << '\n';
            }
            else                                                          // Seq
            {
                unpackSeq(upkSeqOut, i);
                upkfile << upkSeqOut << '\n';
            }
        } while (++i != decText.end());        // If trouble: change "!=" to "<"

        // Update the chunk size and positions (beg & end)
        for (byte t = N_THREADS; t--;)
        {
            in.seekg(endPos);
            in.get(c);
            if (c == (char) 253)
            {
                chunkSizeStr.clear();
                while (in.get(c) && c != (char) 254)    chunkSizeStr += c;

                chunkSize = stoull(chunkSizeStr);
                begPos    = in.tellg();
                endPos    = begPos + (pos_t) chunkSize;
            }
        }
    }

    upkfile.close();
    in.close();
}

/**
 * @brief Unpack: large header
 * @param upkStruct  Unpack structure
 * @param threadID   Thread ID
 */
void FASTA::unpackHL (const unpackfa_s &upkStruct, byte threadID)
{
    pos_t    begPos    = upkStruct.begPos;
    u64      chunkSize = upkStruct.chunkSize;
    ifstream in(DEC_FILENAME);
    string   decText, chunkSizeStr;
    string::iterator i;
    char     c;
    pos_t    endPos;
    ofstream upkfile(UPK_FILENAME+to_string(threadID), std::ios_base::app);
    string   upkHdrOut, upkSeqOut;

    while (in.peek() != EOF)
    {
        in.seekg(begPos);      // Read the file from this position
        // Take a chunk of decrypted file
        decText.clear();
        for (u64 u = chunkSize; u--;) { in.get(c);    decText += c; }
        i = decText.begin();
        endPos = in.tellg();   // Set the end position

        // Unshuffle
        if (shuffled)
        {
            mutxFA.lock();//----------------------------------------------------
            if (VERBOSE && shuffInProgress)    cerr << "Unshuffling...\n";
            shuffInProgress = false;
            mutxFA.unlock();//--------------------------------------------------
    
            unshuffle(i, chunkSize);
        }

        upkfile << THR_ID_HDR + to_string(threadID) << '\n';
        do {
            if (*i == (char) 253)                                         // Hdr
            {
                unpackLarge(upkHdrOut, ++i,
                            upkStruct.XChar_hdr, upkStruct.hdrUnpack);
                upkfile << '>' << upkHdrOut << '\n';
            }
            else                                                          // Seq
            {
                unpackSeq(upkSeqOut, i);
                upkfile << upkSeqOut << '\n';
            }
        } while (++i != decText.end());        // If trouble: change "!=" to "<"

        // Update the chunk size and positions (beg & end)
        for (byte t = N_THREADS; t--;)
        {
            in.seekg(endPos);
            in.get(c);
            if (c == (char) 253)
            {
                chunkSizeStr.clear();
                while (in.get(c) && c != (char) 254)    chunkSizeStr += c;

                chunkSize = stoull(chunkSizeStr);
                begPos    = in.tellg();
                endPos    = begPos + (pos_t) chunkSize;
            }
        }
    }

    upkfile.close();
    in.close();
}

/**
 * @brief Join partially unpacked files
 */
void FASTA::joinUnpackedFiles ()  const
{
    byte     t;                     // For threads
    ifstream upkdFile[N_THREADS];
    string   line;
    for (t = N_THREADS; t--;)    upkdFile[t].open(UPK_FILENAME+to_string(t));
    
    bool prevLineNotThrID;            // If previous line was "THRD=" or not
    while (!upkdFile[0].eof())
    {
        for (t = 0; t != N_THREADS; ++t)
        {
            prevLineNotThrID = false;
            
            while (getline(upkdFile[t], line).good() &&
                   line != THR_ID_HDR+to_string(t))
            {
                if (prevLineNotThrID)
                    cout << '\n';
                cout << line;
                
                prevLineNotThrID = true;
            }
            
            if (prevLineNotThrID)    cout << '\n';
        }
    }
    
    // Close/delete input/output files
    string upkdFileName;
    for (t = N_THREADS; t--;)
    {
        upkdFile[t].close();
        upkdFileName=UPK_FILENAME;    upkdFileName+=to_string(t);
        std::remove(upkdFileName.c_str());
    }
}