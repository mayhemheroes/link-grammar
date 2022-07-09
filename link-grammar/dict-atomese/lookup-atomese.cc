/*
 * lookup-atomese.cc
 *
 * Implement the word-lookup callbacks
 *
 * Copyright (c) 2022 Linas Vepstas <linasvepstas@gmail.com>
 */
#ifdef HAVE_ATOMESE

#include <cstdlib>
#include <opencog/atomspace/AtomSpace.h>
#include <opencog/persist/api/StorageNode.h>
#include <opencog/persist/cog-storage/CogStorage.h>
#include <opencog/persist/file/FileStorage.h>
#include <opencog/persist/rocks/RocksStorage.h>
#include <opencog/persist/sexpr/Sexpr.h>
#include <opencog/nlp/types/atom_types.h>
#undef STRINGIFY

extern "C" {
#include "../link-includes.h"            // For Dictionary
#include "../dict-common/dict-common.h"  // for Dictionary_s
#include "../dict-common/dict-utils.h"   // for size_of_expression()
#include "../dict-ram/dict-ram.h"
#include "lookup-atomese.h"
};

#include "dict-atomese.h"

using namespace opencog;

class Local
{
public:
	bool using_external_as;
	const char* node_str; // (StorageNode \"foo://bar/baz\")
	AtomSpacePtr asp;
	StorageNodePtr stnp;
	Handle linkp; // (Predicate "*-LG connector string-*")
	Handle mikp;  // (Predicate "*-Mutual Info Key cover-section")
};

#define STORAGE_NODE_STRING "storage-node"

/// Shared global
static AtomSpacePtr external_atomspace;
static StorageNodePtr external_storage;

void lg_config_atomspace(AtomSpacePtr asp, StorageNodePtr sto)
{
	external_atomspace = asp;
	external_storage = sto;
}

static const char* get_dict_define(Dictionary dict, const share* namestr)
{
	const char* val_str =
		linkgrammar_get_dict_define(dict, namestr);
	if (nullptr == val_str) return nullptr;

	// Brute-force unescape quotes. Simple, dumb.
	char* unescaped = (char*) alloca(strlen(val_str)+1);
	const char* p = store_str;
	char* q = unescaped;
	while (*p) { if ('\\' != *p) { *q = *p; q++; } p++; }
	*q = 0x0;

	return string_set_add(unescaped, dict->string_set);
}

/// Open a connection to a StorageNode.
bool as_open(Dictionary dict)
{
	const char * stns = get_dict_define(dict, STORAGE_NODE_STRING);
	if (nullptr = stns) return false;

	Local* local = new Local;
	local->node_str = stns;

	// If an external atompsace is specified, then use that.
	if (external_atomspace)
	{
		local->asp = external_atomspace;
		Handle hsn = local->asp->add_atom(external_storage);
		local->stnp = StorageNodeCast(hsn);
		local->using_external_as = true;
	}
	else
	{
		local->asp = createAtomSpace();
		local->using_external_as = false;
	}

	// Create the connector predicate.
	// This will be used to cache LG connector strings.
	local->linkp = local->asp->add_node(PREDICATE_NODE,
		"*-LG connector string-*");

	// This is where costs are stored.
	// XXX FIXME this needs to be configurable.
	local->mikp = local->asp->add_node(PREDICATE_NODE,
		"*-Mutual Info Key cover-section");

	dict->as_server = (void*) local;

	if (local->using_external_as) return true;

	// --------------------
	// If we are here, then we manage our own private AtomSpace.

	if (external_storage)
	{
		Handle hsn = local->asp->add_atom(external_storage);
		local->stnp = StorageNodeCast(hsn);
	}
	else
	{
		Handle hsn = Sexpr::decode_atom(local->node_str);
		hsn = local->asp->add_atom(hsn);
		local->stnp = StorageNodeCast(hsn);
	}

	std::string stone = local->stnp->to_short_string();
	const char * stoname = stone.c_str();

#define SHLIB_CTOR_HACK 1
#ifdef SHLIB_CTOR_HACK
	/* The cast below forces the shared lib constructor to run. */
	/* That's needed to force the factory to get installed. */
	/* We need a more elegant solution to this. */
	Type snt = local->stnp->get_type();
	if (COG_STORAGE_NODE == snt)
		local->stnp = CogStorageNodeCast(local->stnp);
	else if (ROCKS_STORAGE_NODE == snt)
		local->stnp = RocksStorageNodeCast(local->stnp);
	else if (FILE_STORAGE_NODE == snt)
		local->stnp = FileStorageNodeCast(local->stnp);
	else
		printf("Unknown storage %s\n", stoname);
#endif

	local->stnp->open();

	// XXX FIXME -- if we cannot connect, then should hard-fail.
	if (local->stnp->connected())
		printf("Connected to %s\n", stoname);
	else
		printf("Failed to connect to %s\n", stoname);

	return true;
}

/// Close the connection to the cogserver.
void as_close(Dictionary dict)
{
	if (nullptr == dict->as_server) return;
	Local* local = (Local*) (dict->as_server);
	if (not local->using_external_as)
		local->stnp->close();

	delete local;
	dict->as_server = nullptr;

	// Clear the cache as well
	free_dictionary_root(dict);
	dict->num_entries = 0;
}

// ===============================================================

static size_t count_sections(Local* local, const Handle& germ)
{
	// Are there any Sections in the local atomspace?
	size_t nsects = germ->getIncomingSetSizeByType(SECTION);
	if (0 == nsects and local->stnp)
	{
		local->stnp->fetch_incoming_by_type(germ, SECTION);
		local->stnp->barrier();
	}

	nsects = germ->getIncomingSetSizeByType(SECTION);
	return nsects;
}

/// Return true if the given word can be found in the dictionary,
/// else return false.
bool as_boolean_lookup(Dictionary dict, const char *s)
{
	bool found = dict_node_exists_lookup(dict, s);
	if (found) return true;

	if (0 == strcmp(s, LEFT_WALL_WORD))
		s = "###LEFT-WALL###";

	Local* local = (Local*) (dict->as_server);
	Handle wrd = local->asp->add_node(WORD_NODE, s);

	// Are there any Sections for this word in the local atomspace?
	size_t nwrdsects = count_sections(local, wrd);

	// Does this word belong to any classes?
	size_t nclass = wrd->getIncomingSetSizeByType(MEMBER_LINK);
	if (0 == nclass and local->stnp)
	{
		local->stnp->fetch_incoming_by_type(wrd, MEMBER_LINK);
		local->stnp->barrier();
	}

	size_t nclssects = 0;
	for (const Handle& memb : wrd->getIncomingSetByType(MEMBER_LINK))
	{
		const Handle& wcl = memb->getOutgoingAtom(1);
		if (WORD_CLASS_NODE != wcl->get_type()) continue;
		nclssects += count_sections(local, wcl);
	}

printf("duuude as_boolean_lookup for >>%s<< found class=%lu sects=%lu %lu\n",
s, nclass, nwrdsects, nclssects);
	return 0 != (nwrdsects + nclssects);
}

// ===============================================================
/// Mapping LG connectors to AtomSpace connectors, and v.v.
///
/// An LG connector is a string of capital letters. It's generated on
/// the fly. It's associated with a `(Set (Word ..) (Word ...))`. There
/// are two lookup tasks. In this blob of code, when given the Set, we
/// need to find the matching LG string. For users of LG, we have the
/// inverse problem: given the LG string, what's the Set?
///
/// As of just right now, the best solution seems to be the traditional
/// one:
///
///    (Evaluation (Predicate "*-LG connector string-*")
///       (LgConnNode "ABC") (Set (Word ..) (Word ...)))
///
/// As of just right now, the above is held only in the local AtomSpace;
/// it is never written back to storage.

/// int to base-26 capital letters.
static std::string idtostr(uint64_t aid)
{
	std::string s;
	do
	{
		char c = (aid % 26) + 'A';
		s.push_back(c);
	}
	while (0 < (aid /= 26));

	return s;
}

/// Given a `(Set (Word ...) (Word ...))` denoting a link, find the
/// corresponding LgConnNode, if it exists.
static Handle get_lg_conn(const Handle& wset, const Handle& key)
{
	for (const Handle& ev : wset->getIncomingSetByType(EVALUATION_LINK))
	{
		if (ev->getOutgoingAtom(0) == key)
			return ev->getOutgoingAtom(1);
	}
	return Handle::UNDEFINED;
}

/// Return a cached LG-compatible link string.
/// Assisgn a new name, if one does not exist.
static std::string get_linkname(Local* local, const Handle& lnk)
{
	// If we've already cached a connector string for this,
	// just return it.  Else build and cache a string.
	Handle lgc = get_lg_conn(lnk, local->linkp);
	if (lgc)
		return lgc->get_name();

	static uint64_t lid = 0;
	std::string slnk = idtostr(lid++);
	lgc = createNode(LG_CONN_NODE, slnk);
	local->asp->add_link(EVALUATION_LINK, local->linkp, lgc, lnk);
	return slnk;
}

// Cheap hack until c++20 ranges are generally available.
template<typename T>
class reverse {
private:
  T& iterable_;
public:
  explicit reverse(T& iterable) : iterable_{iterable} {}
  auto begin() const { return std::rbegin(iterable_); }
  auto end() const { return std::rend(iterable_); }
};

// Debugging utility
void print_section(Dictionary dict, const Handle& sect)
{
	Local* local = (Local*) (dict->as_server);

	const Handle& germ = sect->getOutgoingAtom(0);
	printf("(%s: ", germ->get_name().c_str());

	const Handle& conseq = sect->getOutgoingAtom(1);
	for (const Handle& ctcr : conseq->getOutgoingSet())
	{
		// The connection target is the first Atom in the connector
		const Handle& tgt = ctcr->getOutgoingAtom(0);
		const Handle& dir = ctcr->getOutgoingAtom(1);
		const std::string& sdir = dir->get_name();
		printf("%s%c ", tgt->get_name().c_str(), sdir[0]);
	}
	printf(") == ");

	bool first = true;
	for (const Handle& ctcr : conseq->getOutgoingSet())
	{
		// The connection target is the first Atom in the connector
		const Handle& tgt = ctcr->getOutgoingAtom(0);
		const Handle& dir = ctcr->getOutgoingAtom(1);

		// The link is the connection of both of these.
		const Handle& lnk = local->asp->add_link(SET_LINK, germ, tgt);

		// Assign an upper-case name to the link.
		std::string slnk = get_linkname(local, lnk);
		const std::string& sdir = dir->get_name();
		if (not first) printf("& ");
		first = false;
		printf("%s%c ", slnk.c_str(), sdir[0]);
	}
	printf("\n");
	fflush(stdout);
}

// ===============================================================

#define INNER_LOOP                                                   \
   std::string slnk;                                                 \
                                                                     \
   /* The connection target is the first Atom in the connector */    \
   const Handle& tgt = ctcr->getOutgoingAtom(0);                     \
                                                                     \
   /* If it and the germ are both words, we have to fake a link */   \
   if (iswrd and WORD_NODE == tgt->get_type()) {                     \
                                                                     \
      /* The link is the connection of both of these. */             \
      const Handle& lnk = local->asp->add_link(SET_LINK, germ, tgt); \
                                                                     \
      /* Assign an upper-case name to the link. */                   \
      slnk = get_linkname(local, lnk);                               \
   } else {                                                          \
      slnk = get_linkname(local, tgt);                               \
   }                                                                 \
   Exp* e = make_connector_node(dict, dict->Exp_pool,                \
                    slnk.c_str(), cdir, false);                      \
   if (nullptr == andex) {                                           \
      andex = e;                                                     \
      continue;                                                      \
   }                                                                 \
   andex = make_and_node(dict->Exp_pool, e, andex);

static Exp* make_exprs(Dictionary dict, const Handle& germ, bool iswrd)
{
	Local* local = (Local*) (dict->as_server);
	Exp* exp = nullptr;

	// Loop over all Sections on the word.
	HandleSeq sects = germ->getIncomingSetByType(SECTION);
	for (const Handle& sect: sects)
	{
// This is harsh. It must be less #define max-disjunct-cost
// in the dict file (currently set to 10 in the demo dict)
#define MISSING_MI -4.0   // This is harsh
		double mi = MISSING_MI;

		// If there's no MI on this secion, just skip it.
		// It's presumably not a valid part of the dataset (???)
		const ValuePtr& mivp = sect->getValue(local->mikp);
		// if (nullptr == mivp) continue;

		// XXX FIXME ... why are the MI's sometimes missing?
		// Did we forget to run a post-processing step?
		if (mivp)
		{
			// MI is the second entry in the list.
			const FloatValuePtr& fmivp = FloatValueCast(mivp);
			mi = fmivp->value()[1];
		}

		Exp* andex = nullptr;

		// The connector sequence the second Atom.
		// Loop over the connectors in the connector sequence.
		// Loop twice, because the atomspace stores connectors
		// in word-order, while LG stores them as distance-from
		// given word.  So we have to reverse the order when
		// building the disjunct.
		const Handle& conseq = sect->getOutgoingAtom(1);
		for (const Handle& ctcr : conseq->getOutgoingSet())
		{
			// The direction is the second Atom in the connector
			const Handle& dir = ctcr->getOutgoingAtom(1);
			char cdir = dir->get_name().c_str()[0];
			if ('+' == cdir) continue;
			INNER_LOOP;
		}
		for (const Handle& ctcr : reverse(conseq->getOutgoingSet()))
		{
			// The direction is the second Atom in the connector
			const Handle& dir = ctcr->getOutgoingAtom(1);
			char cdir = dir->get_name().c_str()[0];
			if ('-' == cdir) continue;
			INNER_LOOP;
		}

		// Cost is minus the MI.
		andex->cost = -mi;

#if DEBUG
		print_section(dict, sect);
		printf("Word %s expression %s\n", ssc, lg_exp_stringify(andex));
#endif

		if (nullptr == exp)
			exp = andex;
		else
			exp = make_or_node(dict->Exp_pool, exp, andex);
	}
	return exp;
}

Dict_node * as_lookup_list(Dictionary dict, const char *s)
{
	// Do we already have this word cached? If so, pull from
	// the cache.
	Dict_node * dn = dict_node_lookup(dict, s);

	if (dn) return dn;

	const char* ssc = string_set_add(s, dict->string_set);
	Local* local = (Local*) (dict->as_server);

	if (0 == strcmp(s, LEFT_WALL_WORD))
		s = "###LEFT-WALL###";

	Handle wrd = local->asp->get_node(WORD_NODE, s);
	if (nullptr == wrd) return nullptr;

	// Get expressions, where the word itself is the germ.
	Exp* exp = make_exprs(dict, wrd, true);

	// Get expressions, where the word is in some class.
	for (const Handle& memb : wrd->getIncomingSetByType(MEMBER_LINK))
	{
		const Handle& wcl = memb->getOutgoingAtom(1);
		if (WORD_CLASS_NODE != wcl->get_type()) continue;

		Exp* clexp = make_exprs(dict, wcl, false);
		if (nullptr == clexp) continue;

printf("duuude as_lookup_list class for >>%s<< had=%d\n", ssc,
size_of_expression(clexp));
		if (nullptr == exp)
			exp = clexp;
		else
			exp = make_or_node(dict->Exp_pool, exp, clexp);
	}

	dn = (Dict_node*) malloc(sizeof(Dict_node));
	memset(dn, 0, sizeof(Dict_node));
	dn->string = ssc;
	dn->exp = exp;

	// Cache the result; avoid repeated lookups.
	dict->root = dict_node_insert(dict, dict->root, dn);
	dict->num_entries++;
printf("duuude as_lookup_list %d for >>%s<< had=%d\n",
dict->num_entries, ssc, size_of_expression(exp));

	// Rebalance the tree every now and then.
	if (0 == dict->num_entries% 30)
	{
		dict->root = dsw_tree_to_vine(dict->root);
		dict->root = dsw_vine_to_tree(dict->root, dict->num_entries);
	}

	// Perform the lookup. We cannot return the dn above, as the
	// as_free_llist() below will delete it, leading to mem corruption.
	dn = dict_node_lookup(dict, ssc);
	return dn;
}

// This is supposed to provide a wild-card lookup.  However,
// There is currently no way to support a wild-card lookup in the
// atomspace: there is no way to ask for all WordNodes that match
// a given regex.  There's no regex predicate... this can be hacked
// around in various elegant and inelegant ways, e.g. adding a
// regex predicate to the AtomSpace. Punt for now. This is used
// only for the !! command in the parser command-line tool.
// XXX FIXME. But low priority.
Dict_node * as_lookup_wild(Dictionary dict, const char *s)
{
	Dict_node * dn = dict_node_wild_lookup(dict, s);
	if (dn) return dn;

	as_lookup_list(dict, s);
	return dict_node_wild_lookup(dict, s);
}

// Zap all the Dict_nodes that we've added earlier.
// This clears out everything hanging on dict->root
// as well as the expression pool.
// And also the local AtomSpace.
//
void as_clear_cache(Dictionary dict)
{
	Local* local = (Local*) (dict->as_server);
	printf("Prior to clear, dict has %d entries, Atomspace has %lu Atoms\n",
		dict->num_entries, local->asp->get_size());

	dict->Exp_pool = pool_new(__func__, "Exp", /*num_elements*/4096,
	                             sizeof(Exp), /*zero_out*/false,
	                             /*align*/false, /*exact*/false);

	// Clear the local AtomSpace too.
	// Easiest way to do this is to just close and reopen
	// the connection.
	const char* storn = local->node_str;

	AtomSpacePtr savea = external_atomspace;
	StorageNodePtr saves = external_storage;
	if (local->using_external_as)
	{
		external_atomspace = local->asp;
		external_storage = local->stnp;
	}

	as_close(dict);
	as_open(dict, storn);
	external_atomspace = savea;
	external_storage = saves;
	as_boolean_lookup(dict, LEFT_WALL_WORD);
}
#endif /* HAVE_ATOMESE */
