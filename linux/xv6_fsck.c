#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <sys/mman.h>

#define ROOTINO 1  // root i-number
#define BSIZE 512  // block size

#define T_DIR       1   // Directory
#define T_FILE      2   // File
#define T_DEV       3   // Special device

// File system super block
struct superblock {
	uint size;         // Size of file system image (blocks)
	uint nblocks;      // Number of data blocks
	uint ninodes;      // Number of inodes.
};

#define NDIRECT 12
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT)

// On-disk inode structure
struct dinode {
	short type;           // File type
	short major;          // Major device number (T_DEV only)
	short minor;          // Minor device number (T_DEV only)
	short nlink;          // Number of links to inode in file system
	uint size;            // Size of file (bytes)
	uint addrs[NDIRECT+1];   // Data block addresses
};

// Inodes per block.
#define IPB           (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i)     ((i) / IPB + 2)

// Bitmap bits per block
#define BPB           (BSIZE*8)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
	ushort inum;
	char name[DIRSIZ];
};

#define no_of_dirent (BSIZE/sizeof(struct dirent))

int main(int argc, char *argv[])
{
	if(argc != 2) {
		fprintf(stderr, "Usage: xv6_fsck file_system_image.\n");
		exit(1);
	}

	int fd = open(argv[1], O_RDONLY);
	if(fd < 0) {
		fprintf(stderr, "image not found.\n");
		exit(1);
	}

	struct stat fs_buf;
	int rc = fstat(fd, &fs_buf);
	assert(rc == 0);

	void *img_ptr = mmap(NULL, fs_buf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	assert(img_ptr != MAP_FAILED);

	struct superblock *sb = (struct superblock*)(img_ptr + BSIZE);
	int inode_valid[sb->ninodes];
	int inode_seen[sb->ninodes];
	int this_is_a_directory[sb->ninodes];

	int start=0;
	int bitmap_initial_position = start/BPB + (sb->ninodes)/IPB + 3;
	int BIP=bitmap_initial_position;
	char *bitmap = (char*)(img_ptr + (BSIZE * bitmap_initial_position));

	int file_ref_in_dir[sb->ninodes];
	int inode_block_valid[sb->size];
	int is_reach[sb->ninodes];
	int dir_num_links[sb->ninodes];

	for(int i = 0; i < sb->ninodes; i++) {
		inode_valid[i] = 0;
		is_reach[i] = 0;
		this_is_a_directory[i] = 0;
		inode_seen[i] = 0;
		dir_num_links[i] = 0;
		file_ref_in_dir[i] = 0;

	}

	for(int i = 0; i < sb->size; i++){
		inode_block_valid[i] = 0;
	}
	struct dinode * temp = (struct dinode*)(img_ptr + (2 * BSIZE));
	for (int i = 0; i < sb->ninodes; i++, temp++){
		if(temp->type == T_DIR)
			this_is_a_directory[i] = 1;
	}

	for(int i = 0; i <= BIP ; i++)
		inode_block_valid[i] = 1;

	struct dinode * curr_inode = (struct dinode*)(img_ptr + (2 * BSIZE));
	for (int i = 0; i < sb->ninodes; i++, curr_inode++) {

		if(curr_inode->type == 0)
			continue;
		if(curr_inode->type != T_DIR && curr_inode->type != T_FILE && curr_inode->type != T_DEV) {
			fprintf(stderr, "ERROR: bad inode.\n");
			close(fd);
			exit(1);
		}
		inode_valid[i] = 1;

		if(i == ROOTINO && (curr_inode->type != T_DIR)) {
			fprintf(stderr, "ERROR: root directory does not exist.\n");
			close(fd);
			exit(1);
		}

		for(int direct_indirect_ptr = 0; direct_indirect_ptr <= NDIRECT; direct_indirect_ptr++) {
			if(direct_indirect_ptr < NDIRECT) {
				if((curr_inode->addrs[direct_indirect_ptr] * BSIZE) >= fs_buf.st_size) {
					fprintf(stderr, "ERROR: bad direct address in inode.\n");
					close(fd);
					exit(1);
				}
				if(curr_inode->addrs[direct_indirect_ptr]) {
					char bitmap_value = *(bitmap +curr_inode->addrs[direct_indirect_ptr]/8);
					bitmap_value = (1 << (curr_inode->addrs[direct_indirect_ptr] % 8) & bitmap_value);
					if(!bitmap_value) {
						fprintf(stderr, "ERROR: address used by inode but marked free in bitmap.\n");
						close(fd);
						exit(1);
					}
					if(*(inode_block_valid + curr_inode->addrs[direct_indirect_ptr]) == 0)
						inode_block_valid[curr_inode->addrs[direct_indirect_ptr]] = 1;
					else {
						fprintf(stderr, "ERROR: direct address used more than once.\n");
						close(fd);
						exit(1);
					}
				}

				if((curr_inode->type == T_DIR)) {
					struct dirent *dirent_ptr = (struct dirent*)(img_ptr + (curr_inode->addrs[direct_indirect_ptr] * BSIZE));
					int check_cur_dir = 0, check_parent = 0;
					for(int z=0 ; z < no_of_dirent ; z++) {
						if(dirent_ptr->inum >= 0 && dirent_ptr->inum < sb->ninodes){
							inode_seen[dirent_ptr->inum] = 1;
							file_ref_in_dir[dirent_ptr->inum]++;
						}
						if(i == 1 && this_is_a_directory[dirent_ptr->inum]){
							is_reach[dirent_ptr->inum] = 1;
						}
						if(is_reach[i] && this_is_a_directory[dirent_ptr->inum]){
							is_reach[dirent_ptr->inum] = 1;
						}
						if(this_is_a_directory[dirent_ptr->inum] == 1 && strcmp(dirent_ptr->name, ".") && strcmp(dirent_ptr->name, "..")) {
							dir_num_links[dirent_ptr->inum]++;
						}
						if(!strcmp(dirent_ptr->name, "..")){
							if(i == 1 && dirent_ptr->inum != 1){
								fprintf(stderr, "ERROR: root directory does not exist.\n");
								close(fd);
								exit(1);
							}
							else
								check_parent = 1;

						}
						if(!strcmp(dirent_ptr->name, ".") && dirent_ptr->inum == i)
							check_cur_dir = 1;

						dirent_ptr++;
					}

					if((direct_indirect_ptr == 0) && (check_cur_dir != 1 || check_parent != 1)){
						fprintf(stderr, "ERROR: directory not properly formatted.\n");
						close(fd);
						exit(1);
					}
				}
			}

			else {
				uint *block_indirect = (uint *)(img_ptr + (curr_inode->addrs[direct_indirect_ptr] * BSIZE));
				inode_block_valid[curr_inode->addrs[direct_indirect_ptr]] = 1;
				for(int k = 0; k < 128; k++) {
					if(((*block_indirect) * BSIZE) >= fs_buf.st_size) {
						fprintf(stderr, "ERROR: bad indirect address in inode.\n");
						close(fd);
						exit(1);
					}
					if(*block_indirect ) {
						char bitmap_value = *(bitmap + *block_indirect/8);
						bitmap_value =  (1 << (*block_indirect % 8) & bitmap_value );
						if(!bitmap_value) {
							fprintf(stderr, "ERROR: address used by inode but marked free in bitmap.\n");
							close(fd);
							exit(1);
						}
						if(*(inode_block_valid + *block_indirect) == 0)
							inode_block_valid[*block_indirect] = 1;
						else{
							fprintf(stderr, "ERROR: indirect address used more than once.\n");
							close(fd);
							exit(1);
						}
					}

					if((curr_inode->type == T_DIR)) {
						struct dirent *dirent_ptr = (struct dirent*)(img_ptr + ((*block_indirect) * BSIZE));
						int check_cur_dir = 0, check_parent = 0;
						for(int z=0 ; z < no_of_dirent ; z++){
							if(dirent_ptr->inum >= 0 && dirent_ptr->inum < sb->ninodes){
								inode_seen[dirent_ptr->inum] = 1;
								file_ref_in_dir[dirent_ptr->inum]++;
							}
							if(i == 1 && this_is_a_directory[dirent_ptr->inum]){
								is_reach[dirent_ptr->inum] = 1;
							}
							if(is_reach[i] && this_is_a_directory[dirent_ptr->inum]){
								is_reach[dirent_ptr->inum] = 1;
							}
							if(this_is_a_directory[dirent_ptr->inum] == 1 && strcmp(dirent_ptr->name, ".") && strcmp(dirent_ptr->name, "..")) {
								dir_num_links[dirent_ptr->inum]++;
							}
							if(!strcmp(dirent_ptr->name, "..")){
								if(i == 1 && dirent_ptr->inum != 1){
									fprintf(stderr, "ERROR: root directory does not exist.\n");
									close(fd);
									exit(1);
								}
								else
									check_parent = 1;

							}
							if(!strcmp(dirent_ptr->name, ".") && dirent_ptr->inum == i)
								check_cur_dir = 1;

							dirent_ptr++;			

						}
						if((direct_indirect_ptr == 0) && (check_cur_dir != 1 || check_parent != 1)){
							fprintf(stderr, "ERROR: directory not properly formatted.\n");
							close(fd);
							exit(1);
						}
					}
					block_indirect++;
				}

			}
		}
	}

	struct dinode *current_inode = (struct dinode*)(img_ptr + (2 * BSIZE));
	for(int inode = 1; inode < sb->ninodes; inode++) {
		current_inode++;
		if(this_is_a_directory[inode] == 1 && !(dir_num_links[inode] == 1 || dir_num_links[inode] == 0)){
			fprintf(stderr, "ERROR: directory appears more than once in file system.\n");
			close(fd);
			exit(1);
		}

		if(current_inode->type == 2 && current_inode->nlink != 0 && current_inode->nlink != file_ref_in_dir[inode]) {
			fprintf(stderr, "ERROR: bad reference count for file.\n");
			close(fd);
			exit(1);
		}

		if(this_is_a_directory[inode] == 1 && is_reach[inode] == 0){
			fprintf(stderr, "ERROR: inaccessible directory exists.\n");
			close(fd);
			exit(1);
		}
		if(inode_seen[inode] == 0 && inode_valid[inode] == 1){
			fprintf(stderr, "ERROR: inode marked use but not found in a directory.\n");
			close(fd);
			exit(1);
		}
		if(inode_seen[inode] == 1 && inode_valid[inode] == 0){
			fprintf(stderr, "ERROR: inode referred to in directory but marked free.\n");
			close(fd);
			exit(1);
		}
	}
	for(int z = 0; z < sb->size; z++){
		if(((inode_block_valid[z] == 0)) && (*(bitmap + z/8) & 1 << (z % 8)) != 0) {
			fprintf(stderr, "ERROR: bitmap marks block in use but it is not in use.\n");
			close(fd);
			exit(1);
		}
	}

	close(fd);
	return 0;
}
