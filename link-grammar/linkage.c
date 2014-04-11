/*************************************************************************/
/* Copyright (c) 2004                                                    */
/* Daniel Sleator, David Temperley, and John Lafferty                    */
/* Copyright 2008, 2009, 2013, 2014 Linas Vepstas                        */
/* All rights reserved                                                   */
/*                                                                       */
/* Use of the link grammar parsing system is subject to the terms of the */
/* license set forth in the LICENSE file included with this software.    */
/* This license allows free redistribution and use in source and binary  */
/* forms, with or without modification, subject to certain conditions.   */
/*                                                                       */
/*************************************************************************/

#include <stdint.h>
#include <stdlib.h>

#include "analyze-linkage.h"
#include "api-structures.h"
#include "disjuncts.h"
#include "extract-links.h"
#include "link-includes.h"
#include "post-process.h"
#include "print.h"
#include "sat-solver/sat-encoder.h"
#include "string-set.h"
#include "structures.h"
#include "word-utils.h"


/***************************************************************
*
* Routines which allow user access to Linkages.
*
****************************************************************/

Linkage linkage_create(size_t k, Sentence sent, Parse_Options opts)
{
	Linkage linkage;

	if (opts->use_sat_solver)
	{
		return sat_create_linkage(k, sent, opts);
	}

	if (k >= sent->num_linkages_post_processed) return NULL;

	extract_links(sent->link_info[k].index, sent->parse_info);

	/* Using exalloc since this is external to the parser itself. */
	linkage = (Linkage) exalloc(sizeof(struct Linkage_s));

	linkage->num_words = sent->length;
	linkage->word = (const char **) exalloc(linkage->num_words*sizeof(char *));
	linkage->sent = sent;
	linkage->opts = opts;
	linkage->info = &sent->link_info[k];
#ifdef USE_FAT_LINKAGES
	linkage->sublinkage = NULL;
	linkage->unionized = false;
	linkage->current = 0;
	linkage->num_sublinkages = 0;
	linkage->dis_con_tree = NULL;

	if (set_has_fat_down(sent))
	{
		extract_fat_linkage(sent, opts, linkage);
	}
	else
#endif /* USE_FAT_LINKAGES */
	{
		extract_thin_linkage(sent, opts, linkage);
	}

	compute_chosen_words(sent, linkage);

	if (sent->dict->postprocessor != NULL)
	{
		linkage_post_process(linkage, sent->dict->postprocessor);
	}

	return linkage;
}

int linkage_get_current_sublinkage(const Linkage linkage)
{
#ifdef USE_FAT_LINKAGES
	return linkage->current;
#else
	return 0;
#endif /* USE_FAT_LINKAGES */
}

int linkage_set_current_sublinkage(Linkage linkage, int index)
{
#ifdef USE_FAT_LINKAGES
	if ((index < 0) ||
		(index >= linkage->num_sublinkages))
	{
		return 0;
	}
	linkage->current = index;
#endif /* USE_FAT_LINKAGES */
	return 1;
}

static void exfree_pp_info(PP_info *ppi)
{
	if (ppi->num_domains > 0)
		exfree((void *) ppi->domain_name, sizeof(const char *) * ppi->num_domains);
	ppi->domain_name = NULL;
	ppi->num_domains = 0;
}

void linkage_delete(Linkage linkage)
{
#ifdef USE_FAT_LINKAGES
	size_t i;
#endif /* USE_FAT_LINKAGES */
	size_t j;
	Sublinkage *s;

	/* Can happen on panic timeout or user error */
	if (NULL == linkage) return;

	exfree((void *) linkage->word, sizeof(const char *) * linkage->num_words);

#ifdef USE_FAT_LINKAGES
	for (i = 0; i < linkage->num_sublinkages; ++i)
#endif /* USE_FAT_LINKAGES */
	{
#ifdef USE_FAT_LINKAGES
		s = &(linkage->sublinkage[i]);
#else
		s = &(linkage->sublinkage);
#endif /* USE_FAT_LINKAGES */
		for (j = 0; j < s->num_links; ++j) {
			exfree_link(s->link[j]);
		}
		exfree(s->link, sizeof(Link*) * s->num_links);
		if (s->pp_info != NULL) {
			for (j = 0; j < s->num_links; ++j) {
				exfree_pp_info(&s->pp_info[j]);
			}
			exfree(s->pp_info, sizeof(PP_info) * s->num_links);
			s->pp_info = NULL;
			post_process_free_data(&s->pp_data);
		}
		if (s->violation != NULL) {
			exfree((void *) s->violation, sizeof(char) * (strlen(s->violation)+1));
		}
	}
#ifdef USE_FAT_LINKAGES
	exfree(linkage->sublinkage, sizeof(Sublinkage)*linkage->num_sublinkages);
#endif /* USE_FAT_LINKAGES */

#ifdef USE_FAT_LINKAGES
	if (linkage->dis_con_tree)
		free_DIS_tree(linkage->dis_con_tree);
#endif /* USE_FAT_LINKAGES */
	exfree(linkage, sizeof(struct Linkage_s));
}

#ifdef USE_FAT_LINKAGES
static int links_are_equal(Link *l, Link *m)
{
	return ((l->l == m->l) && (l->r == m->r) && (strcmp(l->name, m->name)==0));
}

static int link_already_appears(Linkage linkage, Link *link, int a)
{
	int i, j;

	for (i=0; i<a; ++i) {
		for (j=0; j<linkage->sublinkage[i].num_links; ++j) {
			if (links_are_equal(linkage->sublinkage[i].link[j], link)) return true;
		}
	}
	return false;
}

static PP_info excopy_pp_info(PP_info ppi)
{
	PP_info newppi;
	int i;

	newppi.num_domains = ppi.num_domains;
	newppi.domain_name = (const char **) exalloc(sizeof(const char *)*ppi.num_domains);
	for (i=0; i<newppi.num_domains; ++i)
	{
		newppi.domain_name[i] = ppi.domain_name[i];
	}
	return newppi;
}

static Sublinkage unionize_linkage(Linkage linkage)
{
	int i, j, num_in_union=0;
	Sublinkage u;
	Link *link;
	const char *p;

	for (i=0; i<linkage->num_sublinkages; ++i) {
		for (j=0; j<linkage->sublinkage[i].num_links; ++j) {
			link = linkage->sublinkage[i].link[j];
			if (!link_already_appears(linkage, link, i)) num_in_union++;
		}
	}

	u.link = (Link **) exalloc(sizeof(Link *)*num_in_union);
	u.num_links = num_in_union;
	zero_sublinkage(&u);

	u.pp_info = (PP_info *) exalloc(sizeof(PP_info)*num_in_union);
	u.violation = NULL;
	u.num_links = num_in_union;

	num_in_union = 0;

	for (i=0; i<linkage->num_sublinkages; ++i) {
		for (j=0; j<linkage->sublinkage[i].num_links; ++j) {
			link = linkage->sublinkage[i].link[j];
			if (!link_already_appears(linkage, link, i)) {
				u.link[num_in_union] = excopy_link(link);
				u.pp_info[num_in_union] = excopy_pp_info(linkage->sublinkage[i].pp_info[j]);
				if (((p=linkage->sublinkage[i].violation) != NULL) &&
					(u.violation == NULL)) {
					char *s = (char *) exalloc((strlen(p)+1)*sizeof(char));
					strcpy(s, p);
					u.violation = s;
				}
				num_in_union++;
			}
		}
	}

	return u;
}
#endif /* USE_FAT_LINKAGES */

int linkage_compute_union(Linkage linkage)
{
#ifdef USE_FAT_LINKAGES
	int i, num_subs=linkage->num_sublinkages;
	Sublinkage * new_sublinkage, *s;

	if (linkage->unionized) {
		linkage->current = linkage->num_sublinkages-1;
		return 0;
	}
	if (num_subs == 1) {
		linkage->unionized = true;
		return 1;
	}

	new_sublinkage =
		(Sublinkage *) exalloc(sizeof(Sublinkage)*(num_subs+1));

	for (i=0; i<num_subs; ++i) {
		new_sublinkage[i] = linkage->sublinkage[i];
	}
	exfree(linkage->sublinkage, sizeof(Sublinkage)*num_subs);
	linkage->sublinkage = new_sublinkage;

	/* Zero out the new sublinkage, then unionize it. */
	s = &new_sublinkage[num_subs];
	s->link = NULL;
	s->num_links = 0;
	zero_sublinkage(s);
	linkage->sublinkage[num_subs] = unionize_linkage(linkage);

	linkage->num_sublinkages++;

	linkage->unionized = true;
	linkage->current = linkage->num_sublinkages-1;
	return 1;
#else
	return 0;
#endif /* USE_FAT_LINKAGES */
}

int linkage_get_num_sublinkages(const Linkage linkage)
{
	if (!linkage) return 0;
#ifdef USE_FAT_LINKAGES
	return linkage->num_sublinkages;
#else
	return 1;
#endif /* USE_FAT_LINKAGES */
}

int linkage_get_num_words(const Linkage linkage)
{
	if (!linkage) return 0;
	return linkage->num_words;
}

size_t linkage_get_num_links(const Linkage linkage)
{
#ifdef USE_FAT_LINKAGES
	int current;
	if (!linkage) return 0;
	current = linkage->current;
	return linkage->sublinkage[current].num_links;
#else
	if (!linkage) return 0;
	return linkage->sublinkage.num_links;
#endif /* USE_FAT_LINKAGES */
}

static inline Boolean verify_link_index(const Linkage linkage, size_t index)
{
	if (!linkage) return false;
#ifdef USE_FAT_LINKAGES
	if	(index >= linkage->sublinkage[linkage->current].num_links)
#else
	if	(index >= linkage->sublinkage.num_links)
#endif /* USE_FAT_LINKAGES */
	{
		return false;
	}
	return true;
}

int linkage_get_link_length(const Linkage linkage, int index)
{
	Link *link;
	int word_has_link[MAX_SENTENCE];
	size_t i, length;
#ifdef USE_FAT_LINKAGES
	int current = linkage->current;
#endif /* USE_FAT_LINKAGES */

	if (!verify_link_index(linkage, index)) return -1;

	for (i=0; i<linkage->num_words+1; ++i) {
		word_has_link[i] = false;
	}

#ifdef USE_FAT_LINKAGES
	for (i=0; i<linkage->sublinkage[current].num_links; ++i)
	{
		link = linkage->sublinkage[current].link[i];
		word_has_link[link->lw] = true;
		word_has_link[link->rw] = true;
	}
	link = linkage->sublinkage[current].link[index];
#else
	for (i=0; i<linkage->sublinkage.num_links; ++i)
	{
		link = linkage->sublinkage.link[i];
		word_has_link[link->lw] = true;
		word_has_link[link->rw] = true;
	}
	link = linkage->sublinkage.link[index];
#endif /* USE_FAT_LINKAGES */

	length = link->rw - link->lw;
	for (i= link->lw+1; i < link->rw; ++i) {
		if (!word_has_link[i]) length--;
	}
	return length;
}

size_t linkage_get_link_lword(const Linkage linkage, size_t index)
{
	Link *link;
	if (!verify_link_index(linkage, index)) return SIZE_MAX;
#ifdef USE_FAT_LINKAGES
	link = linkage->sublinkage[linkage->current].link[index];
#else
	link = linkage->sublinkage.link[index];
#endif /* USE_FAT_LINKAGES */
	return link->lw;
}

size_t linkage_get_link_rword(const Linkage linkage, size_t index)
{
	Link *link;
	if (!verify_link_index(linkage, index)) return SIZE_MAX;
#ifdef USE_FAT_LINKAGES
	link = linkage->sublinkage[linkage->current].link[index];
#else
	link = linkage->sublinkage.link[index];
#endif /* USE_FAT_LINKAGES */
	return link->rw;
}

const char * linkage_get_link_label(const Linkage linkage, size_t index)
{
	Link *link;
	if (!verify_link_index(linkage, index)) return NULL;
#ifdef USE_FAT_LINKAGES
	link = linkage->sublinkage[linkage->current].link[index];
#else
	link = linkage->sublinkage.link[index];
#endif /* USE_FAT_LINKAGES */
	return link->link_name;
}

const char * linkage_get_link_llabel(const Linkage linkage, size_t index)
{
	Link *link;
	if (!verify_link_index(linkage, index)) return NULL;
#ifdef USE_FAT_LINKAGES
	link = linkage->sublinkage[linkage->current].link[index];
#else
	link = linkage->sublinkage.link[index];
#endif /* USE_FAT_LINKAGES */
	return link->lc->string;
}

const char * linkage_get_link_rlabel(const Linkage linkage, size_t index)
{
	Link *link;
	if (!verify_link_index(linkage, index)) return NULL;
#ifdef USE_FAT_LINKAGES
	link = linkage->sublinkage[linkage->current].link[index];
#else
	link = linkage->sublinkage.link[index];
#endif /* USE_FAT_LINKAGES */
	return link->rc->string;
}

const char ** linkage_get_words(const Linkage linkage)
{
	return linkage->word;
}

Sentence linkage_get_sentence(const Linkage linkage)
{
	return linkage->sent;
}

const char * linkage_get_disjunct_str(const Linkage linkage, int w)
{
	Disjunct *dj;

	if (NULL == linkage) return "";
	if (NULL == linkage->info->disjunct_list_str)
	{
		lg_compute_disjunct_strings(linkage->sent, linkage->info);
	}

	/* dj will be null if the word wasn't used in the parse. */
	dj = linkage->sent->parse_info->chosen_disjuncts[w];
	if (NULL == dj) return "";

	return linkage->info->disjunct_list_str[w];
}

double linkage_get_disjunct_cost(const Linkage linkage, int w)
{
	Disjunct *dj = linkage->sent->parse_info->chosen_disjuncts[w];

	/* dj may be null, if the word didn't participate in the parse. */
	if (dj) return dj->cost;
	return 0.0;
}

double linkage_get_disjunct_corpus_score(const Linkage linkage, int w)
{
	Disjunct *dj = linkage->sent->parse_info->chosen_disjuncts[w];

	/* dj may be null, if the word didn't participate in the parse. */
	if (NULL == dj) return 99.999;

	return lg_corpus_disjunct_score(linkage, w);
}

const char * linkage_get_word(const Linkage linkage, int w)
{
	if (!linkage) return NULL;
	return linkage->word[w];
}

int linkage_unused_word_cost(const Linkage linkage)
{
	/* The sat solver (currently) fails to fill in info */
	if (!linkage->info) return 0;
	return linkage->info->unused_word_cost;
}

double linkage_disjunct_cost(const Linkage linkage)
{
	/* The sat solver (currently) fails to fill in info */
	if (!linkage->info) return 0.0;
	return linkage->info->disjunct_cost;
}

int linkage_is_fat(const Linkage linkage)
{
#ifdef USE_FAT_LINKAGES
	/* The sat solver (currently) fails to fill in info */
	if (!linkage->info) return 0;
	return linkage->info->fat;
#else
	return false;
#endif /* USE_FAT_LINKAGES */
}

int linkage_and_cost(const Linkage linkage)
{
#ifdef USE_FAT_LINKAGES
	/* The sat solver (currently) fails to fill in info */
	if (!linkage->info) return 0;
	return linkage->info->and_cost;
#else
	return 0;
#endif /* USE_FAT_LINKAGES */
}

int linkage_link_cost(const Linkage linkage)
{
	/* The sat solver (currently) fails to fill in info */
	if (!linkage->info) return 0;
	return linkage->info->link_cost;
}

double linkage_corpus_cost(const Linkage linkage)
{
	/* The sat solver (currently) fails to fill in info */
	if (!linkage->info) return 0.0;
	return linkage->info->corpus_cost;
}

int linkage_get_link_num_domains(const Linkage linkage, int index)
{
	PP_info *pp_info;
	if (!verify_link_index(linkage, index)) return -1;
#ifdef USE_FAT_LINKAGES
	pp_info = &linkage->sublinkage[linkage->current].pp_info[index];
#else
	pp_info = &linkage->sublinkage.pp_info[index];
#endif /* USE_FAT_LINKAGES */
	return pp_info->num_domains;
}

const char ** linkage_get_link_domain_names(const Linkage linkage, int index)
{
	PP_info *pp_info;
	if (!verify_link_index(linkage, index)) return NULL;
#ifdef USE_FAT_LINKAGES
	pp_info = &linkage->sublinkage[linkage->current].pp_info[index];
#else
	pp_info = &linkage->sublinkage.pp_info[index];
#endif /* USE_FAT_LINKAGES */
	return pp_info->domain_name;
}

const char * linkage_get_violation_name(const Linkage linkage)
{
#ifdef USE_FAT_LINKAGES
	return linkage->sublinkage[linkage->current].violation;
#else
	return linkage->sublinkage.violation;
#endif /* USE_FAT_LINKAGES */
}

int linkage_is_canonical(const Linkage linkage)
{
#ifdef USE_FAT_LINKAGES
	/* The sat solver (currently) fails to fill in info */
	if (!linkage->info) return true;
	return linkage->info->canonical;
#else
	return true;
#endif /* USE_FAT_LINKAGES */
}

int linkage_is_improper(const Linkage linkage)
{
#ifdef USE_FAT_LINKAGES
	/* The sat solver (currently) fails to fill in info */
	if (!linkage->info) return false;
	return linkage->info->improper_fat_linkage;
#else
	return false;
#endif /* USE_FAT_LINKAGES */
}

int linkage_has_inconsistent_domains(const Linkage linkage)
{
#ifdef USE_FAT_LINKAGES
	/* The sat solver (currently) fails to fill in info */
	if (!linkage->info) return false;
	return linkage->info->inconsistent_domains;
#else
	return false;
#endif /* USE_FAT_LINKAGES */
}

void linkage_post_process(Linkage linkage, Postprocessor * postprocessor)
{
#ifdef USE_FAT_LINKAGES
	int N_sublinkages = linkage_get_num_sublinkages(linkage);
#endif /* USE_FAT_LINKAGES */
	Parse_Options opts = linkage->opts;
	Sentence sent = linkage->sent;
	Sublinkage * subl;
	PP_node * pp;
	size_t j, k;
	D_type_list * d;

#ifdef USE_FAT_LINKAGES
	int i;
	for (i = 0; i < N_sublinkages; ++i)
#endif /* USE_FAT_LINKAGES */
	{
#ifdef USE_FAT_LINKAGES
		subl = &linkage->sublinkage[i];
#else
		subl = &linkage->sublinkage;
#endif /* USE_FAT_LINKAGES */
		if (subl->pp_info != NULL)
		{
			for (j = 0; j < subl->num_links; ++j)
			{
				exfree_pp_info(&subl->pp_info[j]);
			}
			post_process_free_data(&subl->pp_data);
			exfree(subl->pp_info, sizeof(PP_info) * subl->num_links);
		}
		subl->pp_info = (PP_info *) exalloc(sizeof(PP_info) * subl->num_links);
		for (j = 0; j < subl->num_links; ++j)
		{
			subl->pp_info[j].num_domains = 0;
			subl->pp_info[j].domain_name = NULL;
		}
		if (subl->violation != NULL)
		{
			exfree((void *)subl->violation, sizeof(char) * (strlen(subl->violation)+1));
			subl->violation = NULL;
		}

#ifdef USE_FAT_LINKAGES
		if (linkage->info->improper_fat_linkage)
		{
			pp = NULL;
		}
		else
#endif /* USE_FAT_LINKAGES */
		{
			pp = post_process(postprocessor, opts, sent, subl, false);
			/* This can return NULL, for example if there is no
			   post-processor */
		}

		if (pp == NULL)
		{
			for (j = 0; j < subl->num_links; ++j)
			{
				subl->pp_info[j].num_domains = 0;
				subl->pp_info[j].domain_name = NULL;
			}
		}
		else
		{
			for (j = 0; j < subl->num_links; ++j)
			{
				k = 0;
				for (d = pp->d_type_array[j]; d != NULL; d = d->next) k++;
				subl->pp_info[j].num_domains = k;
				if (k > 0)
				{
					subl->pp_info[j].domain_name = (const char **) exalloc(sizeof(const char *)*k);
				}
				k = 0;
				for (d = pp->d_type_array[j]; d != NULL; d = d->next)
				{
					char buff[5];
					sprintf(buff, "%c", d->type);
					subl->pp_info[j].domain_name[k] = string_set_add (buff, sent->string_set);

					k++;
				}
			}
			subl->pp_data = postprocessor->pp_data;
			if (pp->violation != NULL)
			{
				char * s = (char *) exalloc(sizeof(char)*(strlen(pp->violation)+1));
				strcpy(s, pp->violation);
				subl->violation = s;
			}
		}
	}
	post_process_close_sentence(postprocessor);
}
