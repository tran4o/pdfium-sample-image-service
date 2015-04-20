#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <list>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

//#define DOLOG

#include "../fpdfsdk/include/fpdf_dataavail.h"
#include "../fpdfsdk/include/fpdf_ext.h"
#include "../fpdfsdk/include/fpdfformfill.h"
#include "../fpdfsdk/include/fpdftext.h"
#include "../fpdfsdk/include/fpdfview.h"
#include "../core/include/fxcrt/fx_system.h"
#include "v8/include/v8.h"
#include "v8/include/libplatform/libplatform.h"

#ifdef _WIN32
#include <fcntl.h>
#include<io.h>
#endif

#include "LRU.h"

static const int CACHED_PAGES_PER_DOCUMENT = 3;  //5
static const int CACHED_DOCUMENTS = 20; //20
static const int bWidth = 512;
static const int bHeight = 512;
static char tmpBuff[bWidth*bHeight * 4];
static const int RasterCacheMB = 100;		//50MB cache for raster images

#define PDF 0
#define SVG 1
#define JPG 2
#define PNG 3

FILE *logFile = NULL;
void log(std::string s) 
{
	if (logFile == NULL) 
		logFile = fopen("c:\\t\\PDFIUM.LOG","w+");
	else
		logFile = fopen("c:\\t\\PDFIUM.LOG", "a");
	fprintf(logFile, "%s", s.c_str());
	fclose(logFile);
}

int getExt(std::string &name)
{
	int t1 = name.find_last_of(".");
	std::string g1 = name.substr(t1 + 1);
	if (g1.size() == 4) {
		if ((g1[0] == 'J' || g1[0] == 'j') &&
			(g1[1] == 'P' || g1[1] == 'p') &&
			(g1[2] == 'E' || g1[2] == 'e') &&
			(g1[3] == 'G' || g1[3] == 'g'))
		{
			return JPG;
		}
	}
	//------------------------------------------
	if (g1.size() == 3)
	{
		if ((g1[0] == 'P' || g1[0] == 'p') &&
			(g1[1] == 'D' || g1[1] == 'd') &&
			(g1[2] == 'F' || g1[2] == 'f'))
			return PDF;
		else
			if ((g1[0] == 'P' || g1[0] == 'p') &&
				(g1[1] == 'N' || g1[1] == 'n') &&
				(g1[2] == 'G' || g1[2] == 'g'))
				return PNG;
			else
				if ((g1[0] == 'J' || g1[0] == 'j') &&
					(g1[1] == 'P' || g1[1] == 'p') &&
					(g1[2] == 'G' || g1[2] == 'g'))
					return JPG;
				else
					if ((g1[0] == 'S' || g1[0] == 's') &&
						(g1[1] == 'V' || g1[1] == 'v') &&
						(g1[2] == 'G' || g1[2] == 'g'))
						return SVG;
	}
	return -1;
}

class ImageInfo {
public:
	std::string filename;
	double ofsx;
	double ofsy;
	double imgscale;
	int width;	//0 -> not specified
	int height;
	int page;
};

class FileLoader {
public:
	FileLoader(FILE *file);
	FILE *file;
	unsigned long m_Len;
};
FileLoader::FileLoader(FILE *file) {
	this->file = file;
	if (file == NULL)
		return;
	fseek(file, 0, SEEK_END);
	m_Len = ftell(file);
	fseek(file, 0, SEEK_SET);
}
int FileLoaderGetBlock(void* param, unsigned long pos, unsigned char* pBuf, unsigned long size) {
	FileLoader* pLoader = (FileLoader*)param;
	if (pLoader->file == NULL)
		return 0;
	if (pos + size < pos || pos + size > pLoader->m_Len) return 0;
	fseek(pLoader->file, pos, SEEK_SET);
	fread(pBuf, 1, size, pLoader->file);
	return 1;
}
bool FileLoaderIsDataAvail(FX_FILEAVAIL* pThis, size_t offset, size_t size) { return true; }
void FileLoaderAddSegment(FX_DOWNLOADHINTS* pThis, size_t offset, size_t size) {}
//---------------------------------------------------------------------------------------------------------------------------------------------
class PageCacheEntry;
class DocumentCacheEntry
{
public:
	DocumentCacheEntry(const char *filename);
	~DocumentCacheEntry();
	FILE *f;
	FPDF_FORMHANDLE form;
	LRUCache<int, PageCacheEntry*> pageCache;
	FPDF_DOCUMENT doc;
	FPDF_AVAIL pdf_avail;
	int page_count;
	FPDF_FORMFILLINFO form_callbacks;
	IPDF_JSPLATFORM platform_callbacks;
	FileLoader loader;
	FPDF_FILEACCESS file_access;
	FX_FILEAVAIL file_avail;
	FX_DOWNLOADHINTS hints;
};

class PageCacheEntry
{
public:
	PageCacheEntry(DocumentCacheEntry *docEntry, int pageNum) : page(NULL) {
		this->pageNum = pageNum;
		this->docEntry = docEntry;
		page = FPDF_LoadPage(docEntry->doc, pageNum);
		if (page)
		{
			FORM_OnAfterLoadPage(page, docEntry->form);
			FORM_DoPageAAction(page, docEntry->form, FPDFPAGE_AACTION_OPEN);
		}
	}
	~PageCacheEntry() {
		FORM_DoPageAAction(page, docEntry->form, FPDFPAGE_AACTION_CLOSE);
		FORM_OnBeforeClosePage(page, docEntry->form);
		FPDF_ClosePage(page);
	}
	void renderTile(int fx, int fy,int fw,int fh,FPDF_BITMAP bitmap)
	{
		if (page) {
			FPDF_RenderPageBitmap(bitmap, page, fx, fy, fw, fh, 0, 0);
			FPDF_FFLDraw(docEntry->form, bitmap, page, fx, fy, fw, fh, 0, 0);
		}
	}
	int pageNum;
	DocumentCacheEntry *docEntry;
	FPDF_PAGE page;
};

int Form_Alert(IPDF_JSPLATFORM*, FPDF_WIDESTRING, FPDF_WIDESTRING, int, int) {
	return 0;
}


DocumentCacheEntry::DocumentCacheEntry(const char *filename) : pageCache(CACHED_PAGES_PER_DOCUMENT,NULL), f(fopen(filename, "rb")), loader(f) {	//PAGES CACHE SIZE
	if (loader.file == NULL)
		return;
	memset(&platform_callbacks, '\0', sizeof(platform_callbacks));
	platform_callbacks.version = 1;
	platform_callbacks.app_alert = Form_Alert;
	memset(&form_callbacks, '\0', sizeof(form_callbacks));
	form_callbacks.version = 1;
	form_callbacks.m_pJsPlatform = &platform_callbacks;
	memset(&file_access, '\0', sizeof(file_access));
	file_access.m_FileLen = static_cast<unsigned long>(loader.m_Len);
	file_access.m_GetBlock = FileLoaderGetBlock;
	file_access.m_Param = &loader;
	memset(&file_avail, '\0', sizeof(file_avail));
	file_avail.version = 1;
	file_avail.IsDataAvail = FileLoaderIsDataAvail;
	memset(&hints, '\0', sizeof(hints));
	hints.version = 1;
	hints.AddSegment = FileLoaderAddSegment;
	pdf_avail = FPDFAvail_Create(&file_avail, &file_access);
	(void)FPDFAvail_IsDocAvail(pdf_avail, &hints);
	if (!FPDFAvail_IsLinearized(pdf_avail)) {
		doc = FPDF_LoadCustomDocument(&file_access, NULL);
	}
	else {
		doc = FPDFAvail_GetDocument(pdf_avail, NULL);
	}
	(void)FPDF_GetDocPermissions(doc);
	(void)FPDFAvail_IsFormAvail(pdf_avail, &hints);
	form = FPDFDOC_InitFormFillEnvironment(doc, &form_callbacks);
	FPDF_SetFormFieldHighlightColor(form, 0, 0xFFE4DD);
	FPDF_SetFormFieldHighlightAlpha(form, 100);
	int first_page = FPDFAvail_GetFirstPageNum(doc);
	(void)FPDFAvail_IsPageAvail(pdf_avail, first_page, &hints);
	page_count = FPDF_GetPageCount(doc);
	for (int i = 0; i < page_count; ++i) {
		(void)FPDFAvail_IsPageAvail(pdf_avail, i, &hints);
	}
	FORM_DoDocumentJSAction(form);
	FORM_DoDocumentOpenAction(form);
}

DocumentCacheEntry::~DocumentCacheEntry() {
	while (true) 
	{
		PageCacheEntry *t = pageCache.popFront();
		if (t == NULL)
			break;
		delete t;
	}
	if (f == NULL)
		return;
	fclose(f);
	FORM_DoDocumentAAction(form, FPDFDOC_AACTION_WC);
	FPDFDOC_ExitFormFillEnvironment(form);
	FPDF_CloseDocument(doc);
	FPDFAvail_Destroy(pdf_avail);
}
// modifies input string, returns input
std::string& trim_left_in_place(std::string& str) {
	size_t i = 0;
	while (i < str.size() && isspace(str[i])) { ++i; };
	return str.erase(0, i);
}
std::string& trim_right_in_place(std::string& str) {
	size_t i = str.size();
	while (i > 0 && isspace(str[i - 1])) { --i; };
	return str.erase(i, str.size());
}
std::string& trim_in_place(std::string& str) {
	return trim_left_in_place(trim_right_in_place(str));
}
std::string trim_right(std::string str) {
	return trim_right_in_place(str);
}
std::string trim_left(std::string str) {
	return trim_left_in_place(str);
}
std::string trim(std::string str) {
	return trim_left_in_place(trim_right_in_place(str));
}
//-------------------------------------------------------------------------
int flushwrite(const void *_buff,int size,FILE *f) {
	const char *buff = (const char *)_buff;
	int s=0;
	while (true) {
		int r = fwrite(buff+s, 1, size, f);
		if (r <= 0) { if (s != 0) fflush(f); return s; }
		s += r;
		size -= r;
		if (size <= 0) {
			fflush(f);
			return s;
		}
		
	}
	fflush(f);
}
//-------------------------------------------------------------------------
LRUCache<std::string, DocumentCacheEntry*> documentCache(CACHED_DOCUMENTS, NULL);
FPDF_BITMAP bitmap;
char buff[65536];
//--------------------------------------------------------------------------------
// INFO PDF 
//--------------------------------------------------------------------------------
// INFO 
// NAME
// PAGE
bool info() {

	if (!fgets(buff, 65536, stdin)) {
#ifdef DOLOG
		log("INFO ERROR NAME\n");
#endif
		return false;
	}
	std::string name(buff);
	name = trim(name);
#ifdef DOLOG
	log("INFO NAME " + name + "\n");
#endif
	if (!fgets(buff, 65536, stdin)) {
#ifdef DOLOG
		log("INFO ERROR PAGE\n");
#endif
		return false;
	}
	std::string spage(buff);
	spage = trim(spage);
#ifdef DOLOG
	log("INFO PAGE " + spage + "\n");
#endif
	int page = (int)strtod(spage.c_str(), NULL);
	DocumentCacheEntry *de = documentCache.get(name);
	if (de == NULL)
	{
		de = new DocumentCacheEntry(name.c_str());
		DocumentCacheEntry *t = documentCache.put(name, de);
		if (t != NULL) 
		{
			delete t;
		}
	}		
	char tmp[100];
	if (page == -1) {
		memset(tmp, 0, 100);
		sprintf(tmp, "%d", FPDF_GetPageCount(de->doc));
		flushwrite((const void *)&tmp, 100, stdout);
		#ifdef DOLOG
				log("INFO RESULT " + std::string(tmp) + "\n");
		#endif
		return true;
	}
	int w = 0;
	int h = 0;
	if (page < FPDF_GetPageCount(de->doc)) 
	{
		PageCacheEntry *pe = de->pageCache.get(page);
		if (pe == NULL) {
			pe = new PageCacheEntry(de, page);
			PageCacheEntry * t = de->pageCache.put(page, pe);
			if (t != NULL) {
				delete t;
			}
		}
		w = (int)FPDF_GetPageWidth(pe->page);
		h = (int)FPDF_GetPageHeight(pe->page);
	}
	memset(tmp, 0, 100);
	sprintf(tmp, "%d %d", w, h);
	flushwrite((const void *)&tmp,100,stdout);
	#ifdef DOLOG
		log("INFO RESULT " + std::string(tmp) + "\n");
	#endif
	return true;
}

bool ping()
{
	if (!fgets(buff, 65536, stdin)) 
	{
#ifdef DOLOG
		log("INFO ERROR PING\n");
#endif
		return false;
	}
	std::string num(buff);
	num = trim(num);
	char tmp[100];
	memset(tmp, 0, 100);
	sprintf(tmp, "PONG %s",num.c_str());
	flushwrite((const void *)&tmp, 100, stdout);
#ifdef DOLOG
	log(tmp);
	log("\n");
#endif
	return true;
}
//--------------------------------------------------------------------------------
// RENDER PDF 
//--------------------------------------------------------------------------------
// RENDER
// NAME
// PAGE
// x 
// y
// width
// height
bool render()
{
	if (!fgets(buff, 65536, stdin))
		return false;
	std::string name(buff);
	name = trim(name);
	if (!fgets(buff, 65536, stdin))
		return false;
	std::string spage(buff);
	spage = trim(spage);
	int page = (int)strtod(spage.c_str(), NULL);

	if (!fgets(buff, 65536, stdin))
		return false;
	std::string sx(buff);
	sx = trim(sx);
	int x = (int)strtod(sx.c_str(), NULL);

	if (!fgets(buff, 65536, stdin))
		return false;
	std::string sy(buff);
	sy = trim(sy);
	int y = (int)strtod(sy.c_str(), NULL);

	if (!fgets(buff, 65536, stdin))
		return false;
	std::string swidth(buff);
	swidth = trim(swidth);
	int width = (int)strtod(swidth.c_str(), NULL);

	if (!fgets(buff, 65536, stdin))
		return false;
	std::string sheight(buff);
	sheight = trim(sheight);
	int height = (int)strtod(sheight.c_str(), NULL);


	//--------------------------------------------------------------------------------
	unsigned char* buffer = reinterpret_cast<unsigned char*>(FPDFBitmap_GetBuffer(bitmap));
	int stride = FPDFBitmap_GetStride(bitmap);
	for (int ty = 0; ty < bHeight; ty++)
	{
		int ip = ty*stride;
		for (int x = 0; x < bWidth; x++, ip += 4)
		{
			buffer[ip] = 0xFF;		//RED
			buffer[ip + 1] = 0xFF;  //GREEN
			buffer[ip + 2] = 0xFF;	//BLUE
			buffer[ip + 3] = 0x00;	//ALPHA
		}
	}
	DocumentCacheEntry *de = documentCache.get(name);
	if (de == NULL)
	{
		de = new DocumentCacheEntry(name.c_str());
		DocumentCacheEntry *t = documentCache.put(name, de);
		if (t != NULL) {
			delete t;
		}
	}
	PageCacheEntry *pe = de->pageCache.get(page);
	if (pe == NULL) {
		pe = new PageCacheEntry(de, page);
		PageCacheEntry * t = de->pageCache.put(page, pe);
		if (t != NULL) {
			delete t;
		}
	}
	pe->renderTile(x, y, width, height, bitmap);
	//--------------------------------------------
	// OUTPUT IN BGRA
	// NEEDED OUTPUT IS ABGR 
	//const char* buffer = reinterpret_cast<const char*>(FPDFBitmap_GetBuffer(bitmap));
	//int stride = FPDFBitmap_GetStride(bitmap);
	int c = 0;
	for (int ty = 0; ty < bHeight; ty++)
	{
		int ip = ty*stride;
		for (int x = 0; x < bWidth; x++, c += 4, ip += 4)
		{
			tmpBuff[c] = buffer[ip + 3];
			tmpBuff[c + 1] = buffer[ip + 0];
			tmpBuff[c + 2] = buffer[ip + 1];
			tmpBuff[c + 3] = buffer[ip + 2];
		}
	}
	flushwrite(tmpBuff, bWidth*bHeight*4, stdout);
	#ifdef DOLOG
	log("RENDER DONE " + std::to_string(bWidth*bHeight*4) + " BYTES SENT\n");
	#endif
	return true;
}
//--------------------------------------------------------------------------------
// PREVIEW
// NAME
// width
// height
bool preview()
{
	if (!fgets(buff, 65536, stdin)) {
		#ifdef DOLOG
				log("UNABLE TO DO PREVIEW NAME\n");
		#endif
		return false;
	}
	std::string name(buff);
	name = trim(name);
#ifdef DOLOG
	log("PRV NAME " + name + "\n");
#endif
	if (!fgets(buff, 65536, stdin)) {
		#ifdef DOLOG
				log("UNABLE TO DO PREVIEW WIDTH\n");
		#endif
		return false;
	}
	std::string swidth(buff);
	swidth = trim(swidth);
#ifdef DOLOG
	log("PRV WIDTH " + swidth + "\n");
#endif
	int width = (int)strtod(swidth.c_str(), NULL);
	if (!fgets(buff, 65536, stdin)) {
		#ifdef DOLOG
				log("UNABLE TO DO PREVIEW HEIGHT\n");
		#endif
		return false;
	}
	std::string sheight(buff);
	sheight = trim(sheight);
#ifdef DOLOG
	log("PRV HEIGHT " + sheight + "\n");
#endif
	int height = (int)strtod(sheight.c_str(), NULL);
	//--------------------------------------------------------------------------------
	FPDF_BITMAP bitmap = FPDFBitmap_Create(width, height, 1); // ALPHA
	unsigned char* buffer = reinterpret_cast<unsigned char*>(FPDFBitmap_GetBuffer(bitmap));
	int stride = FPDFBitmap_GetStride(bitmap);
	for (int ty = 0; ty < height; ty++)
	{
		int ip = ty*stride;
		for (int x = 0; x < width; x++, ip += 4)
		{
			buffer[ip] = 0xFF;		//RED
			buffer[ip + 1] = 0xFF;  //GREEN
			buffer[ip + 2] = 0xFF;	//BLUE
			buffer[ip + 3] = 0x00;	//ALPHA
		}
	}
	DocumentCacheEntry *de = documentCache.get(name);
	if (de == NULL)
	{
		de = new DocumentCacheEntry(name.c_str());
		DocumentCacheEntry *t = documentCache.put(name, de);
		if (t != NULL) {
			delete t;
		}
	}
	int page = 0;
	PageCacheEntry *pe = de->pageCache.get(page);
	if (pe == NULL) {
		pe = new PageCacheEntry(de, page);
		PageCacheEntry * t = de->pageCache.put(page, pe);
		if (t != NULL) {
			delete t;
		}
	}
	pe->renderTile(0, 0, width, height, bitmap);
	//--------------------------------------------
	unsigned char *tmpBuff = (unsigned char*)malloc(width*height * 4);
	//--------------------------------------------
	// OUTPUT IN BGRA
	// NEEDED OUTPUT IS BGR 
	int c = 0;
	for (int ty = 0; ty < height; ty++)
	{
		int ip = ty*stride;
		for (int x = 0; x < width; x++, c += 4, ip += 4)
		{
			tmpBuff[c] = buffer[ip + 3];
			tmpBuff[c + 1] = buffer[ip + 0];
			tmpBuff[c + 2] = buffer[ip + 1];
			tmpBuff[c + 3] = buffer[ip + 2];
		}
	}
	#ifdef DOLOG
	log("PRV DONE " + std::to_string(width*height*4) + " BYTES\n");
	#endif
	flushwrite(tmpBuff, 4*width*height, stdout);
	free(tmpBuff);
	FPDFBitmap_Destroy(bitmap);
	return true;
}
//-------------------------------------------------------------------------
int main(int argc, const char* argv[])
{
#ifdef _WIN32
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stdin), _O_BINARY);
#endif
	FPDF_InitLibrary();
	//64k read line bufer
	bitmap = FPDFBitmap_Create(bWidth, bHeight, 1); // USE ALPHA
	while (true)
	{
		if (!fgets(buff, 65536, stdin)) 
		{
			#ifdef DOLOG
				log("CONNECTION LOST\n");
			#endif
			break;
		}			
		std::string cmd(buff);
		#ifdef DOLOG
			log("\nCOMMAND '"+trim(cmd)+"'\n");
		#endif
		cmd = trim(cmd);
		if (cmd == "PREVIEW") {
			if (!preview())
				break;
		}
		else if (cmd == "RENDER") {
			if (!render())
				break;
		}
		else if (cmd == "INFO") {
			if (!info())
				break;
		}
		else if (cmd == "PING") {
			if (!ping())
				break;
		}
	}
	FPDFBitmap_Destroy(bitmap);
	FPDF_DestroyLibrary();
}
