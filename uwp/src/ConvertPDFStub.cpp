//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ConvertPDFStub.cpp
//
//  Used by the UWP build when it's configured without PDF support (ES_UWP_PDF=OFF).
//  The PDF converter uses the Poppler library which is licensed under the GPL, so it's
//  optional for the UWP build where it would be linked into the application.
//

#include "ConvertPDF.h"

#include <iostream>

int ConvertPDF::processFile(const std::string /*path*/,
                            const std::string /*mode*/,
                            int /*pageNum*/,
                            int /*width*/,
                            int /*height*/,
                            std::string& /*result*/)
{
    std::cerr << "This build of ES-DE was configured without PDF manual support" << std::endl;
    return -1;
}
