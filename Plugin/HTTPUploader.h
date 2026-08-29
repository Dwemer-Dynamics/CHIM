#include <Windows.h>
#include <WinInet.h>
#include <iostream>
#include <string>
#include <winhttp.h>
#include "Misc.h"

class HTTPUploader {
public:
	static HTTPUploader& getInstance() {
		static HTTPUploader instance;
		return instance;
	}

	std::string UploadFile(std::string data);
    std::string UploadVoiceSample(std::string data, std::string codename, std::string oname);
    std::string UploadVoiceSampleWithText(std::string data, std::string codename, std::string originalName,
                                          std::string referenceText);
	std::string UploadImage(const char* data, int size, std::string hints, int sendMode);
    std::string UploadImagePng(const char* data, int size, std::string hints, int sendMode);
    std::string UploadBookContent(std::string data, std::string title);
    static std::string UploadCSVFile(std::string data, std::string filename, std::string fileType);
    


private:
	HTTPUploader() {}                                       // Private constructor to prevent instantiation
	~HTTPUploader() {}                                      // Private destructor to prevent deletion
	HTTPUploader(const HTTPUploader&) = delete;             // Delete copy constructor
	HTTPUploader& operator=(const HTTPUploader&) = delete;  // Delete assignment operator
	bool WriteToInternet(HINTERNET hInet, const void* Data, DWORD DataSize);

};


