#include "detail/littlefs_volume.h"
#include "lfs.h"
#include <array>
#include <cstring>
#include <limits>
namespace fujinet::platform::pico::detail {
namespace {
struct Volume {
    std::shared_ptr<FlashDevice> device;
    lfs_t lfs{};
    lfs_config config{};
    bool mounted=false;
    explicit Volume(std::shared_ptr<FlashDevice> d):device(std::move(d)) {
        config.context=this;
        config.read=[](const lfs_config* c,lfs_block_t block,lfs_off_t off,void* out,lfs_size_t n) -> int {
            if(block>=flash_length/block_size || off>block_size || n>block_size-off) return LFS_ERR_INVAL;
            auto& v=*static_cast<Volume*>(c->context);
            return v.device->read(block*block_size+off,out,n) ? LFS_ERR_OK : LFS_ERR_IO;
        };
        config.prog=[](const lfs_config* c,lfs_block_t block,lfs_off_t off,const void* data,lfs_size_t n) -> int {
            if(block>=flash_length/block_size || off>block_size || n>block_size-off || off%page_size || n%page_size) return LFS_ERR_INVAL;
            auto& v=*static_cast<Volume*>(c->context);
            return v.device->program(block*block_size+off,data,n) ? LFS_ERR_OK : LFS_ERR_IO;
        };
        config.erase=[](const lfs_config* c,lfs_block_t block) -> int {
            if(block>=flash_length/block_size) return LFS_ERR_INVAL;
            return static_cast<Volume*>(c->context)->device->erase(block*block_size) ? LFS_ERR_OK : LFS_ERR_IO;
        };
        config.sync=[](const lfs_config*) { return 0; };
        config.read_size=1; config.prog_size=page_size; config.block_size=block_size;
        config.block_count=flash_length/block_size; config.block_cycles=500;
        config.cache_size=page_size; config.lookahead_size=32;
    }
    ~Volume() { if(mounted) lfs_unmount(&lfs); }
    bool mount() {
        if(lfs_mount(&lfs,&config)==0) { mounted=true; return true; }
        // Never infer an empty filesystem from a damaged superblock. Scan every
        // byte before permitting the only automatic format path.
        std::array<std::uint8_t,page_size> page;
        for(std::uint32_t offset=0;offset<flash_length;offset+=page_size) {
            if(!device->read(offset,page.data(),page.size())) return false;
            for(auto byte:page) if(byte!=0xff) return false;
        }
        if(lfs_format(&lfs,&config)!=0 || lfs_mount(&lfs,&config)!=0) return false;
        mounted=true; return true;
    }
};
class File final : public fs::IFile {
    std::shared_ptr<Volume> volume_;
    lfs_file_t file_{};
    bool opened_=false;
public:
    explicit File(std::shared_ptr<Volume> v):volume_(std::move(v)) {}
    bool open(const std::string& path,int flags) { opened_=lfs_file_open(&volume_->lfs,&file_,path.c_str(),flags)==0; return opened_; }
    ~File() override { if(opened_) lfs_file_close(&volume_->lfs,&file_); }
    std::size_t read(void* p,std::size_t n) override {
        n=std::min<std::size_t>(n,std::numeric_limits<lfs_size_t>::max());
        const auto r=lfs_file_read(&volume_->lfs,&file_,p,n); return r<0 ? 0 : r;
    }
    std::size_t write(const void* p,std::size_t n) override {
        n=std::min<std::size_t>(n,std::numeric_limits<lfs_size_t>::max());
        const auto r=lfs_file_write(&volume_->lfs,&file_,p,n); return r<0 ? 0 : r;
    }
    bool seek(std::uint64_t offset) override { return offset<=LFS_FILE_MAX && lfs_file_seek(&volume_->lfs,&file_,offset,LFS_SEEK_SET)>=0; }
    std::uint64_t tell() const override { const auto r=lfs_file_tell(&volume_->lfs,const_cast<lfs_file_t*>(&file_)); return r<0 ? 0 : r; }
    bool flush() override { return lfs_file_sync(&volume_->lfs,&file_)==0; }
};
bool valid_path(const std::string& path) {
    if(path.empty() || path.size()>1024 || path.find('\0')!=std::string::npos) return false;
    std::size_t pos=0;
    while(pos<path.size()) {
        const auto end=path.find('/',pos); const auto part=path.substr(pos,end==std::string::npos ? end : end-pos);
        if(part=="..") return false;
        if(end==std::string::npos) break;
        pos=end+1;
    }
    return true;
}
class FileSystem final : public fs::IFileSystem {
    std::shared_ptr<Volume> volume_;
    bool metadata(const std::string& path,lfs_info& info) { return valid_path(path) && lfs_stat(&volume_->lfs,path.c_str(),&info)==0; }
public:
    explicit FileSystem(std::shared_ptr<Volume> v):volume_(std::move(v)) {}
    fs::FileSystemKind kind() const override { return fs::FileSystemKind::LocalFlash; }
    std::string name() const override { return "flash"; }
    bool exists(const std::string& p) override { lfs_info info{}; return metadata(p,info); }
    bool isDirectory(const std::string& p) override { lfs_info info{}; return metadata(p,info) && info.type==LFS_TYPE_DIR; }
    bool createDirectory(const std::string& p) override { return valid_path(p) && lfs_mkdir(&volume_->lfs,p.c_str())==0; }
    bool removeFile(const std::string& p) override { lfs_info i{}; return metadata(p,i) && i.type==LFS_TYPE_REG && lfs_remove(&volume_->lfs,p.c_str())==0; }
    bool removeDirectory(const std::string& p) override { return p!="/" && isDirectory(p) && lfs_remove(&volume_->lfs,p.c_str())==0; }
    bool rename(const std::string& from,const std::string& to) override { return valid_path(from) && valid_path(to) && lfs_rename(&volume_->lfs,from.c_str(),to.c_str())==0; }
    std::unique_ptr<fs::IFile> open(const std::string& path,const char* mode) override {
        if(!valid_path(path) || !mode || !*mode) return nullptr;
        int flags=0;
        if(mode[0]=='r') flags=LFS_O_RDONLY;
        else if(mode[0]=='w') flags=LFS_O_WRONLY|LFS_O_CREAT|LFS_O_TRUNC;
        else if(mode[0]=='a') flags=LFS_O_WRONLY|LFS_O_CREAT|LFS_O_APPEND;
        else return nullptr;
        bool plus=false,binary=false;
        for(const char* c=mode+1;*c;++c) {
            if(*c=='+' && !plus) plus=true;
            else if(*c=='b' && !binary) binary=true;
            else return nullptr;
        }
        if(plus) flags=(flags&~3)|LFS_O_RDWR;
        auto file=std::make_unique<File>(volume_);
        if(!file->open(path,flags)) return nullptr;
        return file;
    }
    bool stat(const std::string& path,fs::FileInfo& out) override {
        lfs_info info{}; if(!metadata(path,info)) return false;
        out={}; out.path=path; out.isDirectory=info.type==LFS_TYPE_DIR; out.sizeBytes=info.size; return true;
    }
    bool listDirectory(const std::string& path,std::vector<fs::FileInfo>& out) override {
        out.clear(); if(!valid_path(path)) return false;
        lfs_dir_t dir{}; if(lfs_dir_open(&volume_->lfs,&dir,path.c_str())<0) return false;
        lfs_info info{}; int result;
        while((result=lfs_dir_read(&volume_->lfs,&dir,&info))>0) {
            if(std::strcmp(info.name,".")==0 || std::strcmp(info.name,"..")==0) continue;
            fs::FileInfo entry{}; entry.path=path+(path.back()=='/' ? "" : "/")+info.name;
            entry.isDirectory=info.type==LFS_TYPE_DIR; entry.sizeBytes=info.size; out.push_back(std::move(entry));
        }
        const auto closed=lfs_dir_close(&volume_->lfs,&dir);
        return result==0 && closed==0;
    }
};
}
std::unique_ptr<fs::IFileSystem> mount_littlefs(std::shared_ptr<FlashDevice> device) {
    if(!device) return nullptr;
    auto volume=std::make_shared<Volume>(std::move(device));
    if(!volume->mount()) return nullptr;
    return std::make_unique<FileSystem>(std::move(volume));
}
}
