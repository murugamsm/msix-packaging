//
//  Copyright (C) 2019 Microsoft.  All rights reserved.
//  See LICENSE file in the project root for full license information.
// 

#include "AppxPackaging.hpp"
#include "AppxPackageWriter.hpp"
#include "MsixErrors.hpp"
#include "Exceptions.hpp"
#include "ContentType.hpp"
#include "Crypto.hpp"

#include <string>
#include <memory>
#include <future>
#include <algorithm>
#include <functional>

#include <zlib.h>

namespace MSIX {

    AppxPackageWriter::AppxPackageWriter(IStream* outputStream) : m_outputStream(outputStream)
    {
        m_state = WriterState::Open;
    }

    // IPackageWriter
    void AppxPackageWriter::Pack(const ComPtr<IDirectoryObject>& from)
    {
        ThrowErrorIf(Error::InvalidState, m_state != WriterState::Open, "Invalid package writer state");
        auto fileMap = from->GetFilesByLastModDate();
        for(const auto& file : fileMap)
        {
            // If any footprint file is present, ignore it. We only require the AppxManifest.xml
            // and any other will be ignored and a new one will be created for the package. 
            if(!IsFootPrintFile(file.second))
            {
                std::string ext = file.second.substr(file.second.find_last_of(".") + 1);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                auto contentType = ContentType::GetContentTypeByExtension(ext);
                ProcessPayloadFile(file.second, from.As<IStorageObject>()->GetFile(file.second),
                    contentType.GetContentType(), contentType.GetCompressionOpt());
            }
        }

        NOTIMPLEMENTED
    }

    // IAppxPackageWriter
    HRESULT STDMETHODCALLTYPE AppxPackageWriter::AddPayloadFile(LPCWSTR fileName, LPCWSTR contentType,
        APPX_COMPRESSION_OPTION compressionOption, IStream *inputStream) noexcept try
    {
        return AddPayloadFile(wstring_to_utf8(fileName).c_str(), wstring_to_utf8(contentType).c_str(), 
            compressionOption, inputStream);
    } CATCH_RETURN();

    HRESULT STDMETHODCALLTYPE AppxPackageWriter::Close(IStream *manifest) noexcept try
    {
        ThrowErrorIf(Error::InvalidState, m_state != WriterState::Open, "Invalid package writer state");
        // TODO: implement
        m_state = WriterState::Closed;
        NOTIMPLEMENTED
    } CATCH_RETURN();

    // IAppxPackageWriterUtf8
    HRESULT STDMETHODCALLTYPE AppxPackageWriter::AddPayloadFile(LPCSTR fileName, LPCSTR contentType,
        APPX_COMPRESSION_OPTION compressionOption, IStream* inputStream) noexcept try
    {
        ThrowErrorIf(Error::InvalidState, m_state != WriterState::Open, "Invalid package writer state");
        ThrowErrorIf(Error::InvalidParameter, IsFootPrintFile(fileName), "Trying to add footprint file to package");
        ComPtr<IStream> stream(inputStream);
        ProcessPayloadFile(fileName, stream, contentType, compressionOption);
        NOTIMPLEMENTED
    } CATCH_RETURN();

    // IAppxPackageWriter3
    HRESULT STDMETHODCALLTYPE AppxPackageWriter::AddPayloadFiles(UINT32 fileCount,
        APPX_PACKAGE_WRITER_PAYLOAD_STREAM* payloadFiles, UINT64 memoryLimit) noexcept try
    {
        ThrowErrorIf(Error::InvalidState, m_state != WriterState::Open, "Invalid package writer state");
        // TODO: use memoryLimit for how many files are going to be added
        for(UINT32 i = 0; i < fileCount; i++)
        {
            std::string fileName = wstring_to_utf8(payloadFiles[i].fileName);
            ThrowErrorIf(Error::InvalidParameter, IsFootPrintFile(fileName), "Trying to add footprint file to package");
            ComPtr<IStream> stream(payloadFiles[i].inputStream);
            std::string contentType = wstring_to_utf8(payloadFiles[i].contentType);
            ProcessPayloadFile(fileName, stream, contentType, payloadFiles[i].compressionOption);
        }

        NOTIMPLEMENTED
    } CATCH_RETURN();

    // IAppxPackageWriter3Utf8
    HRESULT STDMETHODCALLTYPE AppxPackageWriter::AddPayloadFiles(UINT32 fileCount,
        APPX_PACKAGE_WRITER_PAYLOAD_STREAM_UTF8* payloadFiles, UINT64 memoryLimit) noexcept try
    {
        ThrowErrorIf(Error::InvalidState, m_state != WriterState::Open, "Invalid package writer state");
        // TODO: use memoryLimit for how many files are going to be added
        for(UINT32 i = 0; i < fileCount; i++)
        {
            ThrowErrorIf(Error::InvalidParameter, IsFootPrintFile(payloadFiles[i].fileName), "Trying to add footprint file to package");
            ComPtr<IStream> stream(payloadFiles[i].inputStream);
            ProcessPayloadFile(payloadFiles[i].fileName, stream, payloadFiles[i].contentType, payloadFiles[i].compressionOption);
        }

        NOTIMPLEMENTED
    } CATCH_RETURN();

    void AppxPackageWriter::ProcessPayloadFile(const std::string& name, const ComPtr<IStream>& stream,
        const std::string& contentType, APPX_COMPRESSION_OPTION compressionOpt)
    {
        bool isCompress = (compressionOpt != APPX_COMPRESSION_OPTION_NONE );

        // Add content type to [Content Types].xml
        m_contentTypeWriter.AddContentType(name, contentType);

        // TODO: Encode file name, add to lfh to zip and get lfh size

        // This might be called with external IStream implementations. Don't rely on internal implementation of FileStream
        LARGE_INTEGER start = { 0 };
        ULARGE_INTEGER end = { 0 };
        ThrowHrIfFailed(stream->Seek(start, StreamBase::Reference::END, &end));
        ThrowHrIfFailed(stream->Seek(start, StreamBase::Reference::START, nullptr));
        std::uint64_t uncompressedSize = static_cast<std::uint64_t>(end.u.LowPart);

        // Add file to block map
        m_blockMapWriter.AddFile(name, uncompressedSize, 0 /* TODO: change this to lfh size*/);

        std::vector<std::uint8_t> buffer;
        std::uint64_t bytesToRead = uncompressedSize;
        std::uint32_t crc = 0;

        while (bytesToRead > 0)
        {
            // Calculate the size of the next block to add
            std::uint32_t blockSize = (bytesToRead > DefaultBlockSize) ? DefaultBlockSize : static_cast<std::uint32_t>(bytesToRead);
            bytesToRead -= blockSize;

            // read block from stream
            std::vector<std::uint8_t> block;
            block.resize(blockSize);
            ULONG bytesRead;
            ThrowHrIfFailed(stream->Read(static_cast<void*>(block.data()), static_cast<ULONG>(blockSize), &bytesRead));
            ThrowErrorIfNot(Error::FileRead, (static_cast<ULONG>(blockSize) == bytesRead), "Read stream file failed");
            //crc = crc32(crc, block.data(), static_cast<uInt>(block.size()));

            // hash block
            std::vector<std::uint8_t> hash;
            ThrowErrorIfNot(MSIX::Error::SignatureInvalid, 
                MSIX::SHA256::ComputeHash(block.data(), static_cast<uint32_t>(block.size()), hash), 
                "Invalid signature");

            // Add block to blockmap
            m_blockMapWriter.AddBlock(hash, block.size());

            // TODO: compress block if needed
            // if(isCompress)
            //{
            //    std::vector<std::uint8_t> compressedBuffer;
            //    get new compressed block
            //    block.swap(compressedBuffer);
            //}

            buffer.insert(buffer.end(), block.begin(), block.end());
        }

        // TODO: add compressed/uncompressed data to zip

        // TODO: add cdh to zip
    }

    bool AppxPackageWriter::IsFootPrintFile(std::string normalized)
    {
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
        return ((normalized == "appxmanifest.xml") ||
                (normalized == "appxsignature.p7x") ||
                (normalized == "[content_types].xml") ||
                (normalized.rfind("appxmetadata", 0) != std::string::npos) ||
                (normalized.rfind("microsoft.system.package.metadata", 0) != std::string::npos));
    }

}
