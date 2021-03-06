
/* This file is part of minemu
 *
 * Copyright 2010-2011 Erik Bosman <erik@minemu.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "lib.h"
#include "mm.h"
#include "jit_mm.h"

#include "jit.h"
#include "error.h"
#include "jmp_cache.h"
#include "codemap.h"
#include "runtime.h"
#include "threads.h"

#define MAX_CODEMAPS (32768)

/* List of memory maps containing executable data, and their mapping
 * to JIT code (which is allocated in a lazy manner.)
 */
static code_map_t codemaps[MAX_CODEMAPS];
static unsigned n_codemaps = 0;
static long codemap_lock=0;

static void clear_code_map(char *addr, unsigned long len, char *jit_addr)
{
	jit_mem_free(jit_addr); /* PROT_NONE all the things  */
	purge_caches(addr, len); /* remove all cache mappings from each thread's caches */
}

static void del_code_map(unsigned int i)
{
	code_map_t orig = codemaps[i];
	for (; i<n_codemaps; i++)
		codemaps[i] = codemaps[i+1];

	n_codemaps--;

	if (orig.jit_addr)
		clear_code_map(orig.addr, orig.len, orig.jit_addr);
}

code_map_t *find_code_map(char *addr)
{
	mutex_lock(&codemap_lock);

	unsigned int i;
	code_map_t *map = NULL;

	for (i=0; i<n_codemaps; i++)
		if (contains(codemaps[i].addr, codemaps[i].len, addr))
		{
			map = &codemaps[i];
			break;
		}

	mutex_unlock(&codemap_lock);

	return map;
}

code_map_t *find_jit_code_map(char *jit_addr)
{
	mutex_lock(&codemap_lock);

	unsigned int i;
	code_map_t *map = NULL;

	for (i=0; i<n_codemaps; i++)
		if (contains(codemaps[i].jit_addr, codemaps[i].jit_len, jit_addr))
		{
			map = &codemaps[i];
			break;
		}

	mutex_unlock(&codemap_lock);

	return map;
}

static void add_code_map(code_map_t *map)
{
	int i;

	if (n_codemaps >= MAX_CODEMAPS)
		die("Too many codemaps");

	for (i=n_codemaps; i>0; i--)
		if ( (unsigned long)map->addr < (unsigned long)codemaps[i-1].addr )
			codemaps[i] = codemaps[i-1];
		else
			break;

	codemaps[i] = *map;

	n_codemaps++;
}

void add_code_region(char *addr, unsigned long len, unsigned long long inode,
                                                    unsigned long long dev,
                                                    unsigned long mtime,
                                                    unsigned long pgoffset)
{
	del_code_region(addr, len);

	code_map_t map = (code_map_t)
	{
		.addr = addr,
		.len = len,
		.jit_addr = NULL,
		.jit_len = 0,
		.dev = dev,
		.inode = inode,
		.mtime = mtime,
		.pgoffset = pgoffset,
	};

	mutex_lock(&codemap_lock);
	add_code_map(&map);
	mutex_unlock(&codemap_lock);
}

void del_code_region(char *addr, unsigned long len)
{
	mutex_lock(&jit_lock);     /* since we might throw away code */
	mutex_lock(&codemap_lock);
	int i = n_codemaps-1;

	while (i >= 0)
	{
		if (!overlap(addr, len, codemaps[i].addr, codemaps[i].len))
		{
			i--;
			continue;
		}

		code_map_t map = codemaps[i];
		del_code_map(i);

		map.jit_addr = NULL;
		map.jit_len = 0;

		unsigned long start = (unsigned long)addr,
		              end = start + len,
		              o_start = (unsigned long)map.addr,
		              o_end = o_start + map.len;

		if ( start > o_start )
		{
			map.addr = (char *)o_start;
			map.len = start-o_start;
			add_code_map(&map);
		}

		if ( o_end > end )
		{
			map.addr = (char *)end;
			map.len = o_end-end;
			map.pgoffset += (end-o_start)/4096;
			add_code_map(&map);
		}

		i = n_codemaps-1;
	}
	mutex_unlock(&codemap_lock);
	mutex_unlock(&jit_lock);
}

