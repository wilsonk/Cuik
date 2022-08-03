// static size_t file_io_memory_usage = 0;

typedef struct LoadResult {
    bool found;

    size_t length;
    char* data;
} LoadResult;

static LoadResult get_file(bool is_query, const char* path) {
    #ifdef _WIN32
    if (is_query) {
        return (LoadResult){ .found = (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) };
    }

    // actual file reading
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (file == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "error: could not open file '%s'!\n", path);
        return (LoadResult){ .found = false };
    }

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(file, &file_size)) {
        // must be a file stream
        fprintf(stderr, "error: could not check file size of '%s'!\n", path);
        return (LoadResult){ .found = false };
    }

    // normal file with a normal length
    if (file_size.HighPart) {
        fprintf(stderr, "error: file '%s' is too big!\n", path);
        return (LoadResult){ .found = false };
    }

    char* buffer = cuik__valloc((file_size.QuadPart + 16 + 4095) & ~4095);
    DWORD bytes_read;
    if (!ReadFile(file, buffer, file_size.LowPart, &bytes_read, NULL)) {
        fprintf(stderr, "error: could not read file '%s'!\n", path);
        return (LoadResult){ .found = false };
    }

    CloseHandle(file);

    // fat null terminator
    memset(&buffer[file_size.QuadPart], 0, 16);

    //file_io_memory_usage += (file_size.QuadPart + 16);
    //printf("%f MiB of files\n", (double)file_io_memory_usage / 1048576.0);
    return (LoadResult){ .found = true, .length = file_size.QuadPart, buffer };
    #else
    if (is_query) {
        struct stat buffer;
        return (LoadResult){ .found = (stat(path, &buffer) == 0) };
    }

    // actual file reading
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Could not read file: %s\n", path);
        return (LoadResult){ .found = false };
    }

    int descriptor = fileno(file);

    struct stat file_stats;
    if (fstat(descriptor, &file_stats) == -1) {
        fprintf(stderr, "Could not figure out file size: %s\n", path);
        return (LoadResult){ .found = false };
    }

    size_t len = file_stats.st_size;
    char* text = cuik__valloc((len + 16 + 4095) & ~4095);

    fseek(file, 0, SEEK_SET);
    size_t length_read = fread(text, 1, len, file);

    // fat null terminator
    length_read = len;
    memset(&text[length_read], 0, 16);
    fclose(file);

    return (LoadResult){ .found = true, .length = length_read, .data = text };
    #endif
}

CUIK_API bool cuikpp_default_packet_handler(Cuik_CPP* ctx, Cuikpp_Packet* packet) {
    if (packet->tag == CUIKPP_PACKET_GET_FILE || packet->tag == CUIKPP_PACKET_QUERY_FILE) {
        bool is_query = packet->tag == CUIKPP_PACKET_QUERY_FILE;
        LoadResult file = get_file(is_query, packet->file.input_path);

        packet->file.found = file.found;
        packet->file.content_length = file.length;
        packet->file.content = (uint8_t*) file.data;
        return true;
    } else if (packet->tag == CUIKPP_PACKET_CANONICALIZE) {
        #ifdef _WIN32
        char* filepart;
        if (GetFullPathNameA(packet->canonicalize.input_path, FILENAME_MAX, packet->canonicalize.output_path, &filepart) == 0) {
            return false;
        }

        // Convert file paths into something more comfortable
        // The windows file paths are case insensitive
        for (char* p = packet->canonicalize.output_path; *p; p++) {
            if (*p == '\\') {
                *p = '/';
            } else if (*p >= 'A' && *p <= 'Z') {
                *p -= ('A' - 'a');
            }
        }

        return true;
        #else
        return realpath(packet->canonicalize.input_path, packet->canonicalize.output_path) != NULL;
        #endif
    } else {
        return false;
    }
}

CUIK_API void cuikpp_free_default_loaded_file(Cuik_FileEntry* file) {
    cuik__vfree(file->content, (file->content_len + 16 + 4095) & ~4095);
}
