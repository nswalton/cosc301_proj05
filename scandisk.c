#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

int orphans[sizeof(int)*4000];			/* We know global variables are generally bad, but orphans are 
						 * children of the world, so it seemed fitting */

void set_nil(int *num_array, uint16_t max) {
    for (int i = 2; i <= max; i++) {
	num_array[i] = 0;
    }
}

void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
}

int cluster_length(struct direntry *dirent, uint8_t *image_buf, struct bpb33 *bpb)	//Returns the number of clusters linked for a file
{
    uint16_t cluster = getushort(dirent->deStartCluster);
    int count = 0;

    while (is_valid_cluster(cluster, bpb)) {
        cluster = get_fat_entry(cluster, image_buf, bpb);
	count++;
    }
    return count;
}

void shorten_clusters(struct direntry *dirent, uint8_t *image_buf, struct bpb33 *bpb, int maxlen) {
    uint16_t cluster = getushort(dirent->deStartCluster);
    int cluster_in_chain = 1;
    while (is_valid_cluster(cluster, bpb)) {
	uint16_t tempcluster = cluster;
        cluster = get_fat_entry(cluster, image_buf, bpb);

	if (cluster_in_chain == maxlen) {
	    //set cluster to EOF
	    set_fat_entry(tempcluster, CLUST_EOFS, image_buf, bpb);

	}
	if (cluster_in_chain > maxlen) {
	    //set cluster to FREE
	    set_fat_entry(tempcluster, CLUST_FREE, image_buf, bpb);
	}
	cluster_in_chain++;
    }
}

void fix_size(struct direntry *dirent, uint8_t *image_buf, struct bpb33* bpb) {
    
    uint32_t meta_size;
    meta_size = getulong(dirent -> deFileSize);
    int count = cluster_length(dirent, image_buf, bpb);
    int cluster_size = count*512;
    if (meta_size <= (cluster_size) && meta_size >= (cluster_size - 512)) {
	//Do nothing, file size is consistent
	//printf("Clust size for %s is: %d     meta size is: %d\n", dirent -> deName, cluster_size, meta_size);
    }
    else if (meta_size < (cluster_size - 512)) {
	printf("Too many clusters for file %s!\n", dirent -> deName);
	shorten_clusters(dirent, image_buf, bpb, ((meta_size/512) + 1) );
    }
    else {
	printf("The size in the metadata for file %s is larger than it should be!\n", dirent -> deName);
	putulong(dirent -> deFileSize, cluster_size);
    }
}

void count_clusters(struct direntry *dirent, uint8_t *image_buf, struct bpb33 *bpb) {
    uint16_t cluster = getushort(dirent->deStartCluster);
    uint32_t bytes_remaining = getulong(dirent->deFileSize);
    uint16_t cluster_size = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
    
    orphans[cluster] = 1;
    while (is_valid_cluster(cluster, bpb)) {

	//printf("cluster is: %d\n", cluster);
        uint32_t nbytes = bytes_remaining > cluster_size ? cluster_size : bytes_remaining;
        bytes_remaining -= nbytes;
        cluster = get_fat_entry(cluster, image_buf, bpb);
	orphans[cluster] = 1;
    }
}

uint16_t next_dirent(struct direntry *dirent, uint8_t *image_buf, struct bpb33* bpb)
{
    uint16_t followclust = 0;
    int i;
    char name[9];
    char extension[4];
    uint16_t file_cluster;
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);

    if (name[0] == SLOT_EMPTY)
    {
	return followclust;
    }

    /* skip over deleted entries */
    if (((uint8_t)name[0]) == SLOT_DELETED)
    {
	return followclust;
    }

    if (((uint8_t)name[0]) == 0x2E)
    {
	// dot entry ("." or "..")
	// skip it
        return followclust;
    }

    /* names are space padded - remove the spaces */
    for (i = 8; i > 0; i--) 
    {
	if (name[i] == ' ') 
	    name[i] = '\0';
	else 
	    break;
    }

    /* remove the spaces from extensions */
    for (i = 3; i > 0; i--) 
    {
	if (extension[i] == ' ') 
	    extension[i] = '\0';
	else 
	    break;
    }

    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN)
    {
	// ignore any long file name extension entries
    }
    else if ((dirent->deAttributes & ATTR_VOLUME) != 0) 
    {
	return followclust;
	//Do nothing
    } 

    else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0)
    {
        // don't deal with hidden directories; 
	if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN)
        {
            file_cluster = getushort(dirent->deStartCluster);
            followclust = file_cluster;
        }
    }
    else 
    {
        //Regular file
    }


    if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) {			//Directories do not need to be check for consistency
	return followclust;
    }

    fix_size(dirent, image_buf, bpb);
    count_clusters(dirent, image_buf, bpb);

    return followclust;
}

void follow_dir(uint16_t cluster, uint8_t *image_buf, struct bpb33* bpb) {
    while (is_valid_cluster(cluster, bpb))
    {
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
	for ( ; i < numDirEntries; i++)
	{
            
            uint16_t followclust = next_dirent(dirent, image_buf, bpb);
            if (followclust)
                follow_dir(followclust, image_buf, bpb);
            dirent++;
	}
	orphans[cluster] = 1;
	cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}

void traverse_root(uint8_t *image_buf, struct bpb33* bpb)
{
    uint16_t cluster = 0;

    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

    int i = 0;
    for ( ; i < bpb->bpbRootDirEnts; i++)
    {
        uint16_t followclust = next_dirent(dirent, image_buf, bpb);
        if (is_valid_cluster(followclust, bpb))
            follow_dir(followclust, image_buf, bpb);

        dirent++;
    }
}

void find_free(uint8_t *image_buf, struct bpb33* bpb) {
    
    uint16_t max_cluster = (bpb->bpbSectors / bpb->bpbSecPerClust) & FAT12_MASK;
    uint16_t cluster;
    int orphan_clusters[sizeof(int)*max_cluster];
    for (int i = 2; i <= max_cluster; i++) {
	cluster = get_fat_entry(i, image_buf, bpb);
	if (cluster == (FAT12_MASK & CLUST_FREE)) {
	    orphans[i] = 1;
	}
    }
}

void print_orphans(uint16_t max) {
    for (int i = 2; i <= max; i++) {
	if (orphans[i] == 0) {
	    printf("Cluster %d is an orphan, laugh at it for not having parents. HAHA!\n", i);
	}
    }
}

void find_orphan_leaders(uint8_t *image_buf, struct bpb33* bpb, int max) {
    uint16_t cluster;
    int orphan_clones[sizeof(int)*4000];
    for (int i = 2; i <= max; i++) {					//Copy orphans to orphan clones
	orphan_clones[i] = orphans[i];
    }
    for (int i = 2; i <= max; i++) {					//If another orphan points to an orphan, set the latter to 1
	if (orphans[i] == 0) {
	    cluster = get_fat_entry(i, image_buf, bpb);
	    orphan_clones[cluster] = 1;
	}
    }
    for (int i = 2; i <= max; i++) {					//All entries with a 0 are starting cluster of orphan chain
	if (orphan_clones[i] == 0) {
	    printf("All hail cluster %d, king of the orphans\n", i);
	}
    }
}

int main(int argc, char** argv) {
    uint8_t *image_buf;
    int fd;
    struct bpb33* bpb;
    if (argc < 2) {
	usage(argv[0]);
    }

    image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);
    uint16_t max_cluster = (bpb->bpbSectors / bpb->bpbSecPerClust) & FAT12_MASK;
    set_nil(orphans, max_cluster);

    // your code should start here...

    traverse_root(image_buf, bpb);

    find_free(image_buf, bpb);
    print_orphans(max_cluster);
    find_orphan_leaders(image_buf, bpb, max_cluster);

    unmmap_file(image_buf, &fd);
    return 0;
}
