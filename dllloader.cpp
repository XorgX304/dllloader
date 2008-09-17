// tool which exports 3 functions:
//   LoadLibrary
//   GetProcAddress
//   FreeLibrary

#include <stdint.h>

#include "dllloader.h"

#include <stdio.h>
#include <errno.h>

#include <string>
#include <vector>
#include <map>

#ifdef _MSC_VER
#define fseeko _fseeki64
#endif
#ifdef __GNUC__
#define __stdcall __attribute__((stdcall))
#endif
class posixerror {
public:
    posixerror(const std::string& fn, const std::string& name)
        : _err(errno), _fn(fn), _name(name)
    {
    }
    ~posixerror()
    {
        printf("ERROR: %d in %s(%s)\n", _err, _fn.c_str(), _name.c_str());
    }
private:
    // note: on windows this cannot be called '_errno'
    int _err;
    std::string _fn;
    std::string _name;
};
class loadererror {
public:
    loadererror(const std::string& msg)
        : _msg(msg)
    {
    }
    ~loadererror()
    {
        printf("ERROR: %s\n", _msg.c_str());
    }
private:
    std::string _msg;
};
class unimplemented {
public:
    ~unimplemented() { printf("ERROR: unimplemented\n"); }
};
unsigned g_lasterror;

unsigned GetLastError()
{
    return g_lasterror;
}
void SetLastError(unsigned err)
{
    g_lasterror= err;
}

struct mzheader {
    char magic[2];
    uint16_t words[29];
    uint32_t lfanew;
};
struct peheader {
    char magic[4];
    uint16_t cpu;	//The CPU type
    uint16_t objcnt;	//Number of memory objects
    uint32_t timestamp;	//Time EXE file was created/modified
    uint32_t symtaboff;	//Offset to the symbol table

    uint32_t symcount;	//Number of symbols
    uint16_t opthdrsize;	//Optional header size
    uint16_t imageflags;	//Image flags
    // here the opthdr starts.
    uint16_t coffmagic;	//Coff magic number (usually 0x10b)
    uint8_t linkmajor;	//The linker major version number
    uint8_t linkminor;	//The linker minor version number
    uint32_t codesize;	//Sum of sizes of all code sections

    uint32_t initdsize;	//Sum of all initialized data size
    uint32_t uninitdsize;	//Sum of all uninitialized data size
    uint32_t entryrva;	//rva Relative virt. addr. of entry point
    uint32_t codebase;	//rva Address of beginning of code section

    uint32_t database;	//rva Address of beginning of data section
    uint32_t vbase;	//Virtual base address of module
    uint32_t objalign;	//Object Virtual Address align. factor
    uint32_t filealign;	//Image page alignment/truncate factor

    uint16_t osmajor;	//The operating system major ver. no.
    uint16_t osminor;	//The operating system minor ver. no.
    uint16_t usermajor;	//The user major version number
    uint16_t userminor;	//The user minor version number
    uint16_t subsysmajor;	//The subsystem major version number
    uint16_t subsysminor;	//The subsystem minor version number
    uint32_t res1;	//Reserved bytes - must be 0

    uint32_t vsize;	//Virtual size of the entire image
    uint32_t hdrsize;	//Header information size
    uint32_t filechksum;	//Checksum for entire file
    uint16_t subsys;	//The subsystem type
    uint16_t dllflags;	//DLL flags

    uint32_t stackmax;	//Maximum stack size
    uint32_t stackinit;	//Initial committed stack size
    uint32_t heapmax;	//Maximum heap size
    uint32_t heapinit;	//Initial committed heap size

    uint32_t res2;	//Reserved bytes - must be 0
    uint32_t hdrextra;	//Number of extra info units in header

    // followed by 'info' records.

    // offset=((uint8_t*)(&pe->coffmagic))+pe->opthdrsize
    // followed by o32 records
};
struct pe_info {
    uint32_t offset;
    uint32_t size;
};

struct o32_header {
    char name[8];
    uint32_t vsize;
    uint32_t rva;
    uint32_t psize;
    uint32_t dataptr;
    uint32_t realaddr;  // ptr to reloc info
    uint32_t access;
    uint32_t temp3;
    uint32_t flags;
};

class posixfile {
private:
    FILE *_f;
    std::string _name;
public:
    posixfile(const std::string& name)
        : _f(NULL)
    {
        _f= fopen(name.c_str(), "rb");
        if (_f==NULL)
            throw posixerror("fopen", name);
    }
    ~posixfile()
    {
        if (_f)
            fclose(_f);
    }
    void seek(off_t o, int whence=SEEK_SET)
    {
        if (-1==fseeko(_f, o, whence))
            throw posixerror("fseek", _name);
    }
    void readexact(void *p, size_t n)
    {
        //printf("readx %08lx: %lu bytes\n", ftell(_f), n);
        if (1!=fread(p, n, 1, _f))
            throw posixerror("fread", _name);
    }
    int readmax(void *p, size_t nmax)
    {
        int n=fread(p, 1, nmax, _f);
        if (n<0)
            throw posixerror("fread", _name);
        return n;
    }
};

class PEFileInfo {
    struct sectioninfo {
        off_t fileoffset;
        size_t filesize;
        size_t virtualaddress;
        size_t virtualsize;
    };
    struct exportsymbol {
        std::string name;
        unsigned ordinal;
        unsigned virtualaddress;
    };
    struct importsymbol {
        std::string dllname;
        std::string name;
        unsigned ordinal;
        unsigned virtualaddress;
    };
    struct relocinfo {
        size_t virtualaddress;
        int type;
    };
    uint32_t _vbase;
public:
    PEFileInfo(posixfile& f)
        : _f(f)
    {
        f.seek(0);
        mzheader mz;
        f.readexact(&mz, sizeof(mz));

        // read pe header
        f.seek(mz.lfanew);
        peheader pe;
        f.readexact(&pe, sizeof(pe));
        std::vector<pe_info> info(pe.hdrextra>0x10 ? 0x10 : pe.hdrextra);

        _vbase= pe.vbase;
        // read info records
        f.readexact(&info[0], sizeof(pe_info)*info.size());

#define PTR_DIFF(a,b)  ((uint8_t*)(&b)-(uint8_t*)(&a))
        // read o32 records
        f.seek(mz.lfanew+pe.opthdrsize+PTR_DIFF(pe.magic, pe.coffmagic));

        std::vector<o32_header> o32(pe.objcnt);

        f.readexact(&o32[0], sizeof(o32_header)*o32.size());
        _sections.resize(pe.objcnt);
        for (unsigned i=0 ; i<pe.objcnt ; i++)
        {
            _sections[i].fileoffset = o32[i].dataptr;
            _sections[i].filesize   = o32[i].psize;
            _sections[i].virtualaddress= _vbase+o32[i].rva;
            _sections[i].virtualsize= o32[i].vsize;
        }
enum {
    EXP, IMP, RES, EXC, SEC, FIX
};
        if (info[EXP].size)
            read_export_table(info[EXP].offset, info[EXP].size);
        if (info[IMP].size)
            read_import_table(info[IMP].offset, info[IMP].size);
        if (info[FIX].size)
            read_reloc_table(info[FIX].offset, info[FIX].size);
    }

    unsigned sectioncount() const
    {
        return _sections.size();
    }
    const sectioninfo& sectionitem(unsigned i) const
    {
        return _sections[i];
    }

    unsigned importcount() const
    {
        return _imports.size();
    }
    const importsymbol& importitem(unsigned i) const
    {
        return _imports[i];
    }

    unsigned exportcount() const
    {
        return _exports.size();
    }
    const exportsymbol& exportitem(unsigned i) const
    {
        return _exports[i];
    }

    unsigned reloccount() const
    {
        return _relocs.size();
    }
    const relocinfo& relocitem(unsigned i) const
    {
        return _relocs[i];
    }

    size_t minvirtaddr() const
    {
        size_t a= 0;
        for (unsigned i=0 ; i<sectioncount() ; i++)
        {
            if (i==0 || _sections[i].virtualaddress<a)
                a= _sections[i].virtualaddress;
        }
        return a;
    }
    size_t maxvirtaddr() const
    {
        size_t a= 0;
        for (unsigned i=0 ; i<sectioncount() ; i++)
        {
            uint32_t sectionend = _sections[i].virtualaddress+std::max(_sections[i].virtualsize, _sections[i].filesize);
            if (i==0 || sectionend>a)
                a= sectionend;
        }
        return a;
    }
private:
    std::vector<sectioninfo> _sections;
    std::vector<importsymbol> _imports;
    std::vector<exportsymbol> _exports;
    std::vector<relocinfo> _relocs;

    posixfile& _f;

    off_t rva2fileofs(uint32_t rva)
    {
        rva += _vbase;
        for (unsigned i=0 ; i<sectioncount() ; i++)
        {
            if (_sections[i].virtualaddress<=rva && rva<_sections[i].virtualaddress+_sections[i].virtualsize)
                return rva-_sections[i].virtualaddress+_sections[i].fileoffset;
        }
        printf("invalid offset 0x%x requested\n", rva);
        throw loadererror("invalid offset");
    }

    struct export_header {
        uint32_t flags;	// Export table flags, must be 0
        uint32_t timestamp;	// Time export data created
        uint16_t vermajor;	// Major version stamp
        uint16_t verminor;	// Minor version stamp
        uint32_t rva_dllname;	// [rva] Offset to the DLL name
        uint32_t ordbase;	// First valid ordinal
        uint32_t eatcnt;	// Number of EAT entries
        uint32_t namecnt;	// Number of exported names
        uint32_t rva_eat;	// [rva] Export Address Table offset
                                // first ordinal = ordbase
                                // size = eatcnt
        uint32_t rva_name;	// [rva] Export name pointers table off
                                // size = namecnt
        uint32_t rva_ordinal;	// [rva] Export ordinals table offset
                                // size = namecnt
    };
    template<typename T>
    void read_until_zero(uint32_t rva, std::vector<T>&v)
    {
        _f.seek(rva2fileofs(rva));
        int n=0;
        const unsigned chunksize= 256;
        do {
            v.resize(v.size()+chunksize);
            int sizeread= _f.readmax(&v.back()-chunksize+1, sizeof(T)*chunksize);
            n= sizeread/sizeof(T);
            v.resize(v.size()-chunksize+n);
            for (typename std::vector<T>::iterator i=v.end()-n ; i<v.end() ; i++) {
                if (*i==0) {
                    v.resize(i-v.begin());
                    return;
                }
            }
        } while (n>0);
    }

    std::string readstring(uint32_t rva)
    {
        std::vector<char> str;
        read_until_zero(rva, str);
        return std::string(str.begin(), str.end());
    }
    void read_export_table(uint32_t rva, uint32_t size)
    {
        _f.seek(rva2fileofs(rva));
        export_header exphdr;
        _f.readexact(&exphdr, sizeof(exphdr));

        // read export address table
        // entries: if in EXP area : forwarder string
        //          else : exported address
        _f.seek(rva2fileofs(exphdr.rva_eat));
        std::vector<uint32_t> eatlist(exphdr.eatcnt);
        _f.readexact(&eatlist[0], sizeof(uint32_t)*eatlist.size());

        // read export name ptr table
        _f.seek(rva2fileofs(exphdr.rva_name));
        std::vector<uint32_t> entlist(exphdr.namecnt);
        _f.readexact(&entlist[0], sizeof(uint32_t)*entlist.size());

        // read export ordinal table
        _f.seek(rva2fileofs(exphdr.rva_ordinal));
        std::vector<uint16_t> eotlist(exphdr.namecnt);
        _f.readexact(&eotlist[0], sizeof(uint16_t)*eotlist.size());
        //printf("read eot from %08lx: %d entries\n", exphdr.rva_ordinal, eotlist.size());

        //printf("dllname=%s\n", readstring(exphdr.rva_dllname).c_str());

        _exports.resize(eatlist.size());
        for (unsigned i=0 ; i<eatlist.size() ; i++)
        {
            _exports[i].ordinal= i+exphdr.ordbase;
            if (eatlist[i]>=rva && eatlist[i]<rva+size) {
                // todo: handle forward
            }
            else {
                _exports[i].virtualaddress= _vbase+eatlist[i];
            }
        }
        for (unsigned i=0 ; i<entlist.size() ; i++)
        {
            if (eotlist[i]>=_exports.size()) {
                _exports.resize(eotlist[i]+1);
                _exports[eotlist[i]].ordinal= eotlist[i]+exphdr.ordbase;
            }
            _exports[eotlist[i]].name= readstring(entlist[i]);
        }
    }
struct import_header {
    uint32_t rva_lookup;
    uint32_t timestamp;
    uint32_t forwarder;
    uint32_t rva_dllname;
    uint32_t rva_address;
};
    bool isnull(import_header& h)
    {
        return h.rva_lookup==0 && h.timestamp==0 && h.forwarder==0 && h.rva_dllname==0 && h.rva_address==0;
    }
    void read_import_table(uint32_t rva, uint32_t size)
    {

        // read import directory
        for (int nimp=0 ; true ; nimp++) {
            import_header imphdr;
            _f.seek(rva2fileofs(rva)+sizeof(imphdr)*nimp);
            _f.readexact(&imphdr, sizeof(imphdr));
            if (isnull(imphdr))
                break;
            std::vector<uint32_t> ilt;
            read_until_zero(imphdr.rva_lookup, ilt);

            std::string impdllname= readstring(imphdr.rva_dllname);

            for (unsigned i=0 ; i<ilt.size() ; i++)
            {
                importsymbol sym;
                sym.dllname= impdllname;
                sym.virtualaddress= _vbase+imphdr.rva_address+4*i;
                if (ilt[i]&0x80000000) {
                    sym.ordinal= ilt[i]&0x7fffffff;
                }
                else {
                    // todo: handle 'hint'
                    sym.name= readstring(ilt[i]+2);
                }

                _imports.push_back(sym);
            }
        }
    }
    void read_reloc_table(uint32_t rva, uint32_t size)
    {
        _f.seek(rva2fileofs(rva));
        uint32_t roff= rva;
        while (roff<rva+size)
        {
            struct relochdr {
                uint32_t page_rva;
                uint32_t size;
            };
            relochdr hdr;
            _f.readexact(&hdr, sizeof(hdr));

            std::vector<uint16_t> relocs((hdr.size-sizeof(hdr))/sizeof(uint16_t));
            _f.readexact(&relocs[0], hdr.size-sizeof(hdr));
            
            for (unsigned i=0 ; i<relocs.size() ; i++)
            {
                relocinfo r;
                r.virtualaddress= _vbase+hdr.page_rva+(relocs[i]&0xfff);
                r.type= relocs[i]>>12;
                _relocs.push_back(r);
            }
            roff += hdr.size;
        }
    }
};
typedef std::map<std::string,void*> name2ptrmap;
typedef std::map<uint32_t,void*> ord2ptrmap;
typedef std::vector<unsigned char> ByteVector;

class DllModule {
private:
    posixfile _f;
    PEFileInfo _pe;
public:
    DllModule(const std::string& dllname)
        : _f(dllname), _pe(_f)
    {
        load_sections();
        relocate();
        import();
    }
    ~DllModule()
    {
    }
    void load_sections()
    {
        _data.resize(_pe.maxvirtaddr()-_pe.minvirtaddr());
        printf("rva range: %08lx - %08lx\n", _pe.minvirtaddr(), _pe.maxvirtaddr());
        // load sections
        for (unsigned i=0 ; i<_pe.sectioncount() ; i++)
        {
            printf("loading %d: file:%08x:%08lx  rva:%08lx, ofs:%08lx\n",
                    i, uint32_t(_pe.sectionitem(i).fileoffset), _pe.sectionitem(i).filesize,
                    _pe.sectionitem(i).virtualaddress, _pe.sectionitem(i).virtualaddress-_pe.minvirtaddr());
            _f.seek(_pe.sectionitem(i).fileoffset);
            _f.readexact(&_data[_pe.sectionitem(i).virtualaddress-_pe.minvirtaddr()], _pe.sectionitem(i).filesize);
        }

        // process exports 
        for (unsigned i=0 ; i<_pe.exportcount() ; i++)
        {
            if (_pe.exportitem(i).name.empty())
                _exportsbyordinal[_pe.exportitem(i).ordinal]= &_data[_pe.exportitem(i).virtualaddress-_pe.minvirtaddr()];
            else
                _exportsbyname[_pe.exportitem(i).name]= &_data[_pe.exportitem(i).virtualaddress-_pe.minvirtaddr()];
            printf("exp %d %08x ord %4d %s\n", i, _pe.exportitem(i).virtualaddress, _pe.exportitem(i).ordinal, _pe.exportitem(i).name.c_str());
        }

        // process imports
        for (unsigned i=0 ; i<_pe.importcount() ; i++)
        {
            printf("import %d: %08x: ord %4d %s %s\n", i, _pe.importitem(i).virtualaddress, _pe.importitem(i).ordinal, _pe.importitem(i).dllname.c_str(), _pe.importitem(i).name.c_str());
        }
        // relocate
        printf("%d relocs\n", _pe.reloccount());
        for (unsigned i=0 ; i<_pe.reloccount() ; i++)
        {
           // printf("reloc %d: %08lx %d\n", i, _pe.relocitem(i).virtualaddress, _pe.relocitem(i).type);
        }
    }

#define IMAGE_REL_BASED_ABSOLUTE        0
#define IMAGE_REL_BASED_HIGH            1
#define IMAGE_REL_BASED_LOW             2
#define IMAGE_REL_BASED_HIGHLOW         3
#define IMAGE_REL_BASED_HIGHADJ         4
#define IMAGE_REL_BASED_MIPS_JMPADDR    5
#define IMAGE_REL_BASED_SECTION         6
#define IMAGE_REL_BASED_REL32           7
#define IMAGE_REL_BASED_MIPS_JMPADDR16  9
#define IMAGE_REL_BASED_IA64_IMM64      9
#define IMAGE_REL_BASED_DIR64          10
#define IMAGE_REL_BASED_HIGH3ADJ       11

    void relocate()
    {
        uint32_t data_rva= _pe.minvirtaddr();

        uint32_t delta= reinterpret_cast<uint32_t>(&_data[0]-data_rva);

        printf("%08x: <", delta);
        // relocate
        for (unsigned i=0 ; i<_pe.reloccount() ; i++)
        {
            //printf("relocating %08lx: %08x\n", _pe.relocitem(i).virtualaddress, *(uint32_t*)&_data[_pe.relocitem(i).virtualaddress-data_rva]);
            uint8_t *p= &_data[_pe.relocitem(i).virtualaddress-data_rva];
            switch(_pe.relocitem(i).type)
            {
                case IMAGE_REL_BASED_ABSOLUTE:   printf("A"); break;
                case IMAGE_REL_BASED_HIGH:       *(uint16_t*)p += delta>>16;    printf("H"); break;
                case IMAGE_REL_BASED_LOW:        *(uint16_t*)p += delta&0xFFFF; printf("L"); break;
                case IMAGE_REL_BASED_HIGHLOW:    *(uint32_t*)p += delta;        printf("-"); break;
                case IMAGE_REL_BASED_HIGHADJ:      throw unimplemented(); // ?? ... have to re-read description
                default:
                   printf("unhandled fixup type %d\n", _pe.relocitem(i).type);
                   throw unimplemented();
            }
        }
        printf(">\n");

    }
    static void undefined() { printf("unimported\n"); }
    // todo: figure out how to align the stack to 16 bytes here. ( or why malloc needs this alignment )
    static void *__stdcall LocalAlloc(int flag, int size) { return malloc(size); }
    static void *__stdcall LocalFree(void *p) { free(p); return NULL; }
    static void __stdcall SetLastError(uint32_t e) { }
    static bool __stdcall DisableThreadLibraryCalls(void *hmod) { return true; }
    static void dummy() { }
    void import()
    {
        uint32_t data_rva= _pe.minvirtaddr();

        for (unsigned i=0 ; i<_pe.importcount() ; i++)
        {
// DisableThreadLibraryCalls
// SetLastError
// __CppXcptFilter
// __dllonexit
// _adjust_fdiv
// _assert
// _except_handler3
// _initterm
// _onexit

            uint32_t *p= (uint32_t*)&_data[_pe.importitem(i).virtualaddress-data_rva];
            if (_pe.importitem(i).name=="LocalAlloc") *p=(uint32_t)LocalAlloc;
            else if (_pe.importitem(i).name=="LocalFree") *p=(uint32_t)LocalFree;
            else if (_pe.importitem(i).name=="DisableThreadLibraryCalls") *p=(uint32_t)DisableThreadLibraryCalls;
            else if (_pe.importitem(i).name=="SetLastError") *p=(uint32_t)SetLastError;
            else if (_pe.importitem(i).name=="malloc") *p=(uint32_t)malloc;
            else if (_pe.importitem(i).name=="free") *p=(uint32_t)free;
            else if (_pe.importitem(i).name=="_adjust_fdiv") *p=(uint32_t)undefined;
            else *p=(uint32_t)dummy;
            printf("import %d: %08x:=%08x   ord %4d %s %s\n", i, _pe.importitem(i).virtualaddress, *p, _pe.importitem(i).ordinal, _pe.importitem(i).dllname.c_str(), _pe.importitem(i).name.c_str());
        }
    }
    void *getprocbyname(const char *procname) const
    {
        name2ptrmap::const_iterator i= _exportsbyname.find(procname);
        if (i==_exportsbyname.end()) {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return NULL;
        }
        return (*i).second;
    }
    void *getprocbyordinal(unsigned ord) const
    {
        ord2ptrmap::const_iterator i= _exportsbyordinal.find(ord);
        if (i==_exportsbyordinal.end()) {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return NULL;
        }
        return (*i).second;
    }

private:
    name2ptrmap _exportsbyname;
    ord2ptrmap _exportsbyordinal;
    ByteVector _data;
};
HMODULE LoadLibrary(const char*dllname)
{
    try {
        DllModule *dll= new DllModule(dllname);
        return reinterpret_cast<HMODULE>(dll);
    }
    catch(...)
    {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return NULL;
    }
}

void* GetProcAddress(HMODULE hModule, const char*procname)
{
    DllModule *dll= reinterpret_cast<DllModule*>(hModule);
    if (dll==NULL) {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
    unsigned ord= reinterpret_cast<unsigned>(procname);
    if (ord<0x1000)
        return dll->getprocbyordinal(ord);
    return dll->getprocbyname(procname);
}

BOOL FreeLibrary(HMODULE hModule)
{
    DllModule *dll= reinterpret_cast<DllModule*>(hModule);
    if (dll==NULL) {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }
    try {
        delete dll;
        return true;
    }
    catch(...)
    {
        SetLastError(ERROR_GEN_FAILURE);
        return false;
    }
}