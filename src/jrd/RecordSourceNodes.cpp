/*
 * The contents of this file are subject to the Interbase Public
 * License Version 1.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy
 * of the License at http://www.Inprise.com/IPL.html
 *
 * Software distributed under the License is distributed on an
 * "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express
 * or implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code was created by Inprise Corporation
 * and its predecessors. Portions created by Inprise Corporation are
 * Copyright (C) Inprise Corporation.
 *
 * All Rights Reserved.
 * Contributor(s): ______________________________________.
 * Adriano dos Santos Fernandes
 */

#include "firebird.h"
#include "../common/common.h"
#include "../jrd/align.h"
#include "../jrd/RecordSourceNodes.h"
#include "../jrd/DataTypeUtil.h"
#include "../jrd/Optimizer.h"
#include "../jrd/recsrc/RecordSource.h"
#include "../dsql/BoolNodes.h"
#include "../dsql/ExprNodes.h"
#include "../dsql/StmtNodes.h"
#include "../dsql/dsql.h"
#include "../jrd/btr_proto.h"
#include "../jrd/cmp_proto.h"
#include "../common/dsc_proto.h"
#include "../jrd/met_proto.h"
#include "../jrd/opt_proto.h"
#include "../jrd/par_proto.h"
#include "../dsql/ddl_proto.h"
#include "../dsql/gen_proto.h"
#include "../dsql/pass1_proto.h"

using namespace Firebird;
using namespace Jrd;


static MapNode* parseMap(thread_db* tdbb, CompilerScratch* csb, USHORT stream);
static SSHORT strcmpSpace(const char* p, const char* q);
static void processSource(thread_db* tdbb, CompilerScratch* csb, RseNode* rse,
	RecordSourceNode* source, BoolExprNode** boolean, RecordSourceNodeStack& stack);
static void processMap(thread_db* tdbb, CompilerScratch* csb, MapNode* map, Format** inputFormat);
static void genDeliverUnmapped(thread_db* tdbb, BoolExprNodeStack* deliverStack, MapNode* map,
	BoolExprNodeStack* parentStack, UCHAR shellStream);
static void markIndices(CompilerScratch::csb_repeat* csbTail, SSHORT relationId);
static dsql_nod* resolveUsingField(DsqlCompilerScratch* dsqlScratch, dsql_str* name,
	DsqlNodStack& stack, const dsql_nod* flawedNode, const TEXT* side, dsql_ctx*& ctx);
static void sortIndicesBySelectivity(CompilerScratch::csb_repeat* csbTail);


//--------------------


SortNode* SortNode::copy(thread_db* tdbb, NodeCopier& copier)
{
	SortNode* newSort = FB_NEW(*tdbb->getDefaultPool()) SortNode(*tdbb->getDefaultPool());
	newSort->unique = unique;

	for (NestConst<ValueExprNode>* i = expressions.begin(); i != expressions.end(); ++i)
		newSort->expressions.add(copier.copy(tdbb, *i));

	newSort->descending = descending;
	newSort->nullOrder = nullOrder;

	return newSort;
}

SortNode* SortNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	for (NestConst<ValueExprNode>* i = expressions.begin(); i != expressions.end(); ++i)
		DmlNode::doPass1(tdbb, csb, i->getAddress());

	return this;
}

SortNode* SortNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	for (NestConst<ValueExprNode>* i = expressions.begin(); i != expressions.end(); ++i)
		(*i)->nodFlags |= ExprNode::FLAG_VALUE;

	for (NestConst<ValueExprNode>* i = expressions.begin(); i != expressions.end(); ++i)
		ExprNode::doPass2(tdbb, csb, i->getAddress());

	return this;
}

bool SortNode::computable(CompilerScratch* csb, SSHORT stream, bool idx_use,
	bool allowOnlyCurrentStream)
{
	for (NestConst<ValueExprNode>* i = expressions.begin(); i != expressions.end(); ++i)
	{
		if (!(*i)->computable(csb, stream, idx_use, allowOnlyCurrentStream))
			return false;
	}

	return true;
}

void SortNode::findDependentFromStreams(const OptimizerRetrieval* optRet,
	SortedStreamList* streamList)
{
	for (NestConst<ValueExprNode>* i = expressions.begin(); i != expressions.end(); ++i)
		(*i)->findDependentFromStreams(optRet, streamList);
}


//--------------------


MapNode* MapNode::copy(thread_db* tdbb, NodeCopier& copier)
{
	MapNode* newMap = FB_NEW(*tdbb->getDefaultPool()) MapNode(*tdbb->getDefaultPool());

	NestConst<ValueExprNode>* target = targetList.begin();

	for (NestConst<ValueExprNode>* source = sourceList.begin();
		 source != sourceList.end();
		 ++source, ++target)
	{
		newMap->sourceList.add(copier.copy(tdbb, *source));
		newMap->targetList.add(copier.copy(tdbb, *target));
	}

	return newMap;
}

MapNode* MapNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	NestConst<ValueExprNode>* target = targetList.begin();

	for (NestConst<ValueExprNode>* source = sourceList.begin();
		 source != sourceList.end();
		 ++source, ++target)
	{
		DmlNode::doPass1(tdbb, csb, source->getAddress());
		DmlNode::doPass1(tdbb, csb, target->getAddress());
	}

	return this;
}

MapNode* MapNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	NestConst<ValueExprNode>* target = targetList.begin();

	for (NestConst<ValueExprNode>* source = sourceList.begin();
		 source != sourceList.end();
		 ++source, ++target)
	{
		ExprNode::doPass2(tdbb, csb, source->getAddress());
		ExprNode::doPass2(tdbb, csb, target->getAddress());
	}

	return this;
}


//--------------------


// Parse a relation reference.
RelationSourceNode* RelationSourceNode::parse(thread_db* tdbb, CompilerScratch* csb,
	SSHORT blrOp, bool parseContext)
{
	SET_TDBB(tdbb);

	// Make a relation reference node

	RelationSourceNode* node = FB_NEW(*tdbb->getDefaultPool()) RelationSourceNode(
		*tdbb->getDefaultPool());

	// Find relation either by id or by name
	string* aliasString = NULL;
	MetaName name;

	switch (blrOp)
	{
		case blr_rid:
		case blr_rid2:
		{
			const SSHORT id = csb->csb_blr_reader.getWord();

			if (blrOp == blr_rid2)
			{
				aliasString = FB_NEW(csb->csb_pool) string(csb->csb_pool);
				PAR_name(csb, *aliasString);
			}

			if (!(node->relation = MET_lookup_relation_id(tdbb, id, false)))
				name.printf("id %d", id);

			break;
		}

		case blr_relation:
		case blr_relation2:
		{
			PAR_name(csb, name);

			if (blrOp == blr_relation2)
			{
				aliasString = FB_NEW(csb->csb_pool) string(csb->csb_pool);
				PAR_name(csb, *aliasString);
			}

			node->relation = MET_lookup_relation(tdbb, name);
			break;
		}

		default:
			fb_assert(false);
	}

	if (!node->relation)
		PAR_error(csb, Arg::Gds(isc_relnotdef) << Arg::Str(name), false);

	// if an alias was passed, store with the relation

	if (aliasString)
		node->alias = *aliasString;

	// Scan the relation if it hasn't already been scanned for meta data

	if ((!(node->relation->rel_flags & REL_scanned) || (node->relation->rel_flags & REL_being_scanned)) &&
		((node->relation->rel_flags & REL_force_scan) || !(csb->csb_g_flags & csb_internal)))
	{
		node->relation->rel_flags &= ~REL_force_scan;
		MET_scan_relation(tdbb, node->relation);
	}
	else if (node->relation->rel_flags & REL_sys_triggers)
		MET_parse_sys_trigger(tdbb, node->relation);

	// generate a stream for the relation reference, assuming it is a real reference

	if (parseContext)
	{
		node->stream = PAR_context(csb, &node->context);
		fb_assert(node->stream <= MAX_STREAMS);

		csb->csb_rpt[node->stream].csb_relation = node->relation;
		csb->csb_rpt[node->stream].csb_alias = aliasString;

		if (csb->csb_g_flags & csb_get_dependencies)
			PAR_dependency(tdbb, csb, node->stream, (SSHORT) -1, "");
	}
	else
		delete aliasString;

	return node;
}

bool RelationSourceNode::dsqlMatch(const ExprNode* other, bool ignoreMapCast) const
{
	const RelationSourceNode* o = other->as<RelationSourceNode>();
	return o && dsqlContext == o->dsqlContext;
}

// Generate blr for a relation reference.
void RelationSourceNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	const dsql_rel* relation = dsqlContext->ctx_relation;

	// if this is a trigger or procedure, don't want relation id used

	if (DDL_ids(dsqlScratch))
	{
		dsqlScratch->appendUChar(dsqlContext->ctx_alias.hasData() ? blr_rid2 : blr_rid);
		dsqlScratch->appendUShort(relation->rel_id);
	}
	else
	{
		dsqlScratch->appendUChar(dsqlContext->ctx_alias.hasData() ? blr_relation2 : blr_relation);
		dsqlScratch->appendMetaString(relation->rel_name.c_str());
	}

	if (dsqlContext->ctx_alias.hasData())
		dsqlScratch->appendMetaString(dsqlContext->ctx_alias.c_str());

	GEN_stuff_context(dsqlScratch, dsqlContext);
}

RelationSourceNode* RelationSourceNode::copy(thread_db* tdbb, NodeCopier& copier)
{
	if (!copier.remap)
		BUGCHECK(221);	// msg 221 (CMP) copy: cannot remap

	RelationSourceNode* newSource = FB_NEW(*tdbb->getDefaultPool()) RelationSourceNode(
		*tdbb->getDefaultPool());

	// Last entry in the remap contains the the original stream number.
	// Get that stream number so that the flags can be copied
	// into the newly created child stream.

	const int relativeStream = stream ? copier.remap[stream - 1] : stream;
	newSource->stream = copier.csb->nextStream();
	copier.remap[stream] = (UCHAR) newSource->stream;

	newSource->context = context;
	newSource->relation = relation;
	newSource->view = view;

	CompilerScratch::csb_repeat* element = CMP_csb_element(copier.csb, newSource->stream);
	element->csb_relation = newSource->relation;
	element->csb_view = newSource->view;
	element->csb_view_stream = copier.remap[0];

	/** If there was a parent stream no., then copy the flags
		from that stream to its children streams. (Bug 10164/10166)
		For e.g.
		consider a view V1 with 2 streams
				stream #1 from table T1
			stream #2 from table T2
		consider a procedure P1 with 2 streams
				stream #1  from table X
			stream #2  from view V1

		During pass1 of procedure request, the engine tries to expand
		all the views into their base tables. It creates a compiler
		scratch block which initially looks like this
				stream 1  -------- X
				stream 2  -------- V1
			while expanding V1 the engine calls copy() with nod_relation.
			A new stream 3 is created. Now the CompilerScratch looks like
				stream 1  -------- X
				stream 2  -------- V1  map [2,3]
				stream 3  -------- T1
			After T1 stream has been created the flags are copied from
			stream #1 because V1's definition said the original stream
			number for T1 was 1. However since its being merged with
			the procedure request, stream #1 belongs to a different table.
			The flags should be copied from stream 2 i.e. V1. We can get
			this info from variable remap.

			Since we didn't do this properly before, V1's children got
			tagged with whatever flags X possesed leading to various
			errors.

			We now store the proper stream no in relativeStream and
			later use it to copy the flags. -Sudesh (03/05/99)
	**/

	copier.csb->csb_rpt[newSource->stream].csb_flags |=
		copier.csb->csb_rpt[relativeStream].csb_flags & csb_no_dbkey;

	return newSource;
}

void RelationSourceNode::ignoreDbKey(thread_db* tdbb, CompilerScratch* csb) const
{
	csb->csb_rpt[stream].csb_flags |= csb_no_dbkey;
	const CompilerScratch::csb_repeat* tail = &csb->csb_rpt[stream];
	const jrd_rel* relation = tail->csb_relation;

	if (relation)
	{
		CMP_post_access(tdbb, csb, relation->rel_security_name,
			(tail->csb_view) ? tail->csb_view->rel_id : (view ? view->rel_id : 0),
			SCL_read, SCL_object_table, relation->rel_name);
	}
}

void RelationSourceNode::pass1Source(thread_db* tdbb, CompilerScratch* csb, RseNode* rse,
	BoolExprNode** boolean, RecordSourceNodeStack& stack)
{
	stack.push(this);	// Assume that the source will be used. Push it on the final stream stack.

	// We have a view or a base table;
	// prepare to check protection of relation when a field in the stream of the
	// relation is accessed.

	jrd_rel* const parentView = csb->csb_view;
	const USHORT viewStream = csb->csb_view_stream;

	jrd_rel* relationView = relation;
	CMP_post_resource(&csb->csb_resources, relationView, Resource::rsc_relation, relationView->rel_id);
	view = parentView;

	CompilerScratch::csb_repeat* const element = CMP_csb_element(csb, stream);
	element->csb_view = parentView;
	fb_assert(viewStream <= MAX_STREAMS);
	element->csb_view_stream = (UCHAR) viewStream;

	// in the case where there is a parent view, find the context name

	if (parentView)
	{
		const ViewContexts& ctx = parentView->rel_view_contexts;
		const USHORT key = context;
		size_t pos;

		if (ctx.find(key, pos))
		{
			element->csb_alias = FB_NEW(csb->csb_pool)
				string(csb->csb_pool, ctx[pos]->vcx_context_name);
		}
	}

	// check for a view - if not, nothing more to do

	RseNode* viewRse = relationView->rel_view_rse;
	if (!viewRse)
		return;

	// we've got a view, expand it

	DEBUG;
	stack.pop();
	UCHAR* map = CMP_alloc_map(tdbb, csb, stream);

	AutoSetRestore<USHORT> autoRemapVariable(&csb->csb_remap_variable,
		(csb->csb_variables ? csb->csb_variables->count() : 0) + 1);
	AutoSetRestore<jrd_rel*> autoView(&csb->csb_view, relationView);
	AutoSetRestore<USHORT> autoViewStream(&csb->csb_view_stream, stream);

	// We don't expand the view in two cases:
	// 1) If the view has a projection, sort, first/skip or explicit plan.
	// 2) If it's part of an outer join.

	if (rse->rse_jointype || // viewRse->rse_jointype || ???
		viewRse->rse_sorted || viewRse->rse_projection || viewRse->rse_first ||
		viewRse->rse_skip || viewRse->rse_plan)
	{
		NodeCopier copier(csb, map);
		RseNode* copy = viewRse->copy(tdbb, copier);
		DEBUG;
		doPass1(tdbb, csb, &copy);
		stack.push(copy);
		DEBUG;
		return;
	}

	// ASF: Below we start to do things when viewRse->rse_projection is not NULL.
	// But we should never come here, as the code above returns in this case.

	// if we have a projection which we can bubble up to the parent rse, set the
	// parent rse to our projection temporarily to flag the fact that we have already
	// seen one so that lower-level views will not try to map their projection; the
	// projection will be copied and correctly mapped later, but we don't have all
	// the base streams yet

	if (viewRse->rse_projection)
		rse->rse_projection = viewRse->rse_projection;

	// disect view into component relations

	NestConst<RecordSourceNode>* arg = viewRse->rse_relations.begin();
	for (const NestConst<RecordSourceNode>* const end = viewRse->rse_relations.end(); arg != end; ++arg)
	{
		// this call not only copies the node, it adds any streams it finds to the map
		NodeCopier copier(csb, map);
		RecordSourceNode* node = (*arg)->copy(tdbb, copier);

		// Now go out and process the base table itself. This table might also be a view,
		// in which case we will continue the process by recursion.
		processSource(tdbb, csb, rse, node, boolean, stack);
	}

	// When there is a projection in the view, copy the projection up to the query RseNode.
	// In order to make this work properly, we must remap the stream numbers of the fields
	// in the view to the stream number of the base table. Note that the map at this point
	// contains the stream numbers of the referenced relations, since it was added during the call
	// to copy() above. After the copy() below, the fields in the projection will reference the
	// base table(s) instead of the view's context (see bug #8822), so we are ready to context-
	// recognize them in doPass1() - that is, replace the field nodes with actual field blocks.

	if (viewRse->rse_projection)
	{
		NodeCopier copier(csb, map);
		rse->rse_projection = viewRse->rse_projection->copy(tdbb, copier);
		doPass1(tdbb, csb, rse->rse_projection.getAddress());
	}

	// if we encounter a boolean, copy it and retain it by ANDing it in with the
	// boolean on the parent view, if any

	if (viewRse->rse_boolean)
	{
		NodeCopier copier(csb, map);
		BoolExprNode* node = copier.copy(tdbb, viewRse->rse_boolean);

		doPass1(tdbb, csb, &node);

		if (*boolean)
		{
			// The order of the nodes here is important! The
			// boolean from the view must appear first so that
			// it gets expanded first in pass1.

			BinaryBoolNode* andNode = FB_NEW(csb->csb_pool) BinaryBoolNode(csb->csb_pool, blr_and);
			andNode->arg1 = node;
			andNode->arg2 = *boolean;

			*boolean = andNode;
		}
		else
			*boolean = node;
	}
}

void RelationSourceNode::pass2Rse(thread_db* tdbb, CompilerScratch* csb)
{
	fb_assert(stream <= MAX_STREAMS);
	csb->csb_rpt[stream].csb_flags |= csb_active;

	pass2(tdbb, csb);
}

RecordSource* RelationSourceNode::compile(thread_db* tdbb, OptimizerBlk* opt, bool /*innerSubStream*/)
{
	fb_assert(stream <= MAX_UCHAR);
	fb_assert(opt->beds[0] < MAX_STREAMS && opt->beds[0] < MAX_UCHAR); // debug check
	//if (opt->beds[0] >= MAX_STREAMS) // all builds check
	//	ERR_post(Arg::Gds(isc_too_many_contexts));

	opt->beds[++opt->beds[0]] = (UCHAR) stream;

	// we have found a base relation; record its stream
	// number in the streams array as a candidate for
	// merging into a river

	// TMN: Is the intention really to allow streams[0] to overflow?
	// I must assume that is indeed not the intention (not to mention
	// it would make code later on fail), so I added the following fb_assert.
	fb_assert(opt->compileStreams[0] < MAX_STREAMS && opt->compileStreams[0] < MAX_UCHAR);

	opt->compileStreams[++opt->compileStreams[0]] = (UCHAR) stream;

	if (opt->rse->rse_jointype == blr_left)
		opt->outerStreams.add(stream);

	// if we have seen any booleans or sort fields, we may be able to
	// use an index to optimize them; retrieve the current format of
	// all indices at this time so we can determine if it's possible
	// AB: if a parentStack was available and conjunctCount was 0
	// then no indices where retrieved. Added also OR check on
	// parentStack below. SF BUG # [ 508594 ]

	if (opt->conjunctCount || opt->rse->rse_sorted || opt->rse->rse_aggregate || opt->parentStack)
	{
		if (relation && !relation->rel_file && !relation->isVirtual())
		{
			opt->opt_csb->csb_rpt[stream].csb_indices =
				BTR_all(tdbb, relation, &opt->opt_csb->csb_rpt[stream].csb_idx, relation->getPages(tdbb));
			sortIndicesBySelectivity(&opt->opt_csb->csb_rpt[stream]);
			markIndices(&opt->opt_csb->csb_rpt[stream], relation->rel_id);
		}
		else
			opt->opt_csb->csb_rpt[stream].csb_indices = 0;

		const Format* format = CMP_format(tdbb, opt->opt_csb, stream);
		opt->opt_csb->csb_rpt[stream].csb_cardinality =
			OPT_getRelationCardinality(tdbb, relation, format);
	}

	return NULL;
}


//--------------------


// Parse an procedural view reference.
ProcedureSourceNode* ProcedureSourceNode::parse(thread_db* tdbb, CompilerScratch* csb,
	SSHORT blrOp)
{
	SET_TDBB(tdbb);

	jrd_prc* procedure = NULL;
	string* aliasString = NULL;
	QualifiedName name;

	switch (blrOp)
	{
		case blr_pid:
		case blr_pid2:
		{
			const SSHORT pid = csb->csb_blr_reader.getWord();

			if (blrOp == blr_pid2)
			{
				aliasString = FB_NEW(csb->csb_pool) string(csb->csb_pool);
				PAR_name(csb, *aliasString);
			}

			if (!(procedure = MET_lookup_procedure_id(tdbb, pid, false, false, 0)))
				name.identifier.printf("id %d", pid);

			break;
		}

		case blr_procedure:
		case blr_procedure2:
		case blr_procedure3:
		case blr_procedure4:
		{
			if (blrOp == blr_procedure3 || blrOp == blr_procedure4)
				PAR_name(csb, name.package);

			PAR_name(csb, name.identifier);

			if (blrOp == blr_procedure2 || blrOp == blr_procedure4)
			{
				aliasString = FB_NEW(csb->csb_pool) string(csb->csb_pool);
				PAR_name(csb, *aliasString);
			}

			procedure = MET_lookup_procedure(tdbb, name, false);

			break;
		}

		default:
			fb_assert(false);
	}

	if (!procedure)
		PAR_error(csb, Arg::Gds(isc_prcnotdef) << Arg::Str(name.toString()));

	if (procedure->prc_type == prc_executable)
	{
		const string name = procedure->getName().toString();

		if (tdbb->getAttachment()->att_flags & ATT_gbak_attachment)
			PAR_warning(Arg::Warning(isc_illegal_prc_type) << Arg::Str(name));
		else
			PAR_error(csb, Arg::Gds(isc_illegal_prc_type) << Arg::Str(name));
	}

	ProcedureSourceNode* node = FB_NEW(*tdbb->getDefaultPool()) ProcedureSourceNode(
		*tdbb->getDefaultPool());

	node->procedure = procedure->getId();
	node->stream = PAR_context(csb, &node->context);

	csb->csb_rpt[node->stream].csb_procedure = procedure;
	csb->csb_rpt[node->stream].csb_alias = aliasString;

	PAR_procedure_parms(tdbb, csb, procedure, node->in_msg.getAddress(),
		node->sourceList.getAddress(), node->targetList.getAddress(), true);

	if (csb->csb_g_flags & csb_get_dependencies)
		PAR_dependency(tdbb, csb, node->stream, (SSHORT) -1, "");

	return node;
}

bool ProcedureSourceNode::dsqlAggregateFinder(AggregateFinder& visitor)
{
	// Check if relation is a procedure.
	if (dsqlContext->ctx_procedure)
	{
		// Check if a aggregate is buried inside the input parameters.
		return visitor.visit(&dsqlContext->ctx_proc_inputs);
	}

	return false;
}

bool ProcedureSourceNode::dsqlAggregate2Finder(Aggregate2Finder& visitor)
{
	return false;
}

bool ProcedureSourceNode::dsqlInvalidReferenceFinder(InvalidReferenceFinder& visitor)
{
	// If relation is a procedure, check if the parameters are valid.
	if (dsqlContext->ctx_procedure)
		return visitor.visit(&dsqlContext->ctx_proc_inputs);

	return false;
}

bool ProcedureSourceNode::dsqlSubSelectFinder(SubSelectFinder& visitor)
{
	return false;
}

bool ProcedureSourceNode::dsqlFieldFinder(FieldFinder& visitor)
{
	return false;
}

bool ProcedureSourceNode::dsqlFieldRemapper(FieldRemapper& visitor)
{
	// Check if relation is a procedure.
	if (dsqlContext->ctx_procedure)
		visitor.visit(&dsqlContext->ctx_proc_inputs);	// Remap the input parameters.

	return false;
}

bool ProcedureSourceNode::dsqlMatch(const ExprNode* other, bool ignoreMapCast) const
{
	const ProcedureSourceNode* o = other->as<ProcedureSourceNode>();
	return o && dsqlContext == o->dsqlContext;
}

// Generate blr for a procedure reference.
void ProcedureSourceNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	const dsql_prc* procedure = dsqlContext->ctx_procedure;

	// If this is a trigger or procedure, don't want procedure id used.

	if (DDL_ids(dsqlScratch))
	{
		dsqlScratch->appendUChar(dsqlContext->ctx_alias.hasData() ? blr_pid2 : blr_pid);
		dsqlScratch->appendUShort(procedure->prc_id);
	}
	else
	{
		if (procedure->prc_name.package.hasData())
		{
			dsqlScratch->appendUChar(dsqlContext->ctx_alias.hasData() ? blr_procedure4 : blr_procedure3);
			dsqlScratch->appendMetaString(procedure->prc_name.package.c_str());
			dsqlScratch->appendMetaString(procedure->prc_name.identifier.c_str());
		}
		else
		{
			dsqlScratch->appendUChar(dsqlContext->ctx_alias.hasData() ? blr_procedure2 : blr_procedure);
			dsqlScratch->appendMetaString(procedure->prc_name.identifier.c_str());
		}
	}

	if (dsqlContext->ctx_alias.hasData())
		dsqlScratch->appendMetaString(dsqlContext->ctx_alias.c_str());

	GEN_stuff_context(dsqlScratch, dsqlContext);

	dsql_nod* inputs = dsqlContext->ctx_proc_inputs;

	if (inputs)
	{
		dsqlScratch->appendUShort(inputs->nod_count);

		dsql_nod* const* ptr = inputs->nod_arg;

		for (const dsql_nod* const* const end = ptr + inputs->nod_count; ptr < end; ptr++)
			GEN_expr(dsqlScratch, *ptr);
	}
	else
		dsqlScratch->appendUShort(0);
}

ProcedureSourceNode* ProcedureSourceNode::copy(thread_db* tdbb, NodeCopier& copier)
{
	if (!copier.remap)
		BUGCHECK(221);	// msg 221 (CMP) copy: cannot remap

	ProcedureSourceNode* newSource = FB_NEW(*tdbb->getDefaultPool()) ProcedureSourceNode(
		*tdbb->getDefaultPool());

	// dimitr: See the appropriate code and comment in NodeCopier (in nod_argument).
	// We must copy the message first and only then use the new pointer to
	// copy the inputs properly.
	newSource->in_msg = copier.copy(tdbb, in_msg);

	{	// scope
		AutoSetRestore<MessageNode*> autoMessage(&copier.message, newSource->in_msg);
		newSource->sourceList = copier.copy(tdbb, sourceList);
		newSource->targetList = copier.copy(tdbb, targetList);
	}

	newSource->stream = copier.csb->nextStream();
	copier.remap[stream] = (UCHAR) newSource->stream;
	newSource->context = context;
	newSource->procedure = procedure;
	newSource->view = view;
	CompilerScratch::csb_repeat* element = CMP_csb_element(copier.csb, newSource->stream);
	// SKIDDER: Maybe we need to check if we really found a procedure?
	element->csb_procedure = MET_lookup_procedure_id(tdbb, newSource->procedure, false, false, 0);
	element->csb_view = newSource->view;
	element->csb_view_stream = copier.remap[0];

	copier.csb->csb_rpt[newSource->stream].csb_flags |= copier.csb->csb_rpt[stream].csb_flags & csb_no_dbkey;

	return newSource;
}

RecordSourceNode* ProcedureSourceNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, sourceList.getAddress());
	doPass1(tdbb, csb, targetList.getAddress());
	doPass1(tdbb, csb, in_msg.getAddress());
	return this;
}

void ProcedureSourceNode::pass1Source(thread_db* tdbb, CompilerScratch* csb, RseNode* /*rse*/,
	BoolExprNode** /*boolean*/, RecordSourceNodeStack& stack)
{
	stack.push(this);	// Assume that the source will be used. Push it on the final stream stack.

	pass1(tdbb, csb);

	jrd_prc* const proc = MET_lookup_procedure_id(tdbb, procedure, false, false, 0);
	CMP_post_procedure_access(tdbb, csb, proc);
	CMP_post_resource(&csb->csb_resources, proc, Resource::rsc_procedure, proc->getId());

	jrd_rel* const parentView = csb->csb_view;
	const USHORT viewStream = csb->csb_view_stream;
	view = parentView;

	CompilerScratch::csb_repeat* const element = CMP_csb_element(csb, stream);
	element->csb_view = parentView;
	fb_assert(viewStream <= MAX_STREAMS);
	element->csb_view_stream = (UCHAR) viewStream;

	// in the case where there is a parent view, find the context name

	if (parentView)
	{
		const ViewContexts& ctx = parentView->rel_view_contexts;
		const USHORT key = context;
		size_t pos;

		if (ctx.find(key, pos))
		{
			element->csb_alias = FB_NEW(csb->csb_pool) string(
				csb->csb_pool, ctx[pos]->vcx_context_name);
		}
	}
}

RecordSourceNode* ProcedureSourceNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ExprNode::doPass2(tdbb, csb, sourceList.getAddress());
	ExprNode::doPass2(tdbb, csb, targetList.getAddress());
	ExprNode::doPass2(tdbb, csb, in_msg.getAddress());
	return this;
}

void ProcedureSourceNode::pass2Rse(thread_db* tdbb, CompilerScratch* csb)
{
	fb_assert(stream <= MAX_STREAMS);
	csb->csb_rpt[stream].csb_flags |= csb_active;

	pass2(tdbb, csb);
}

RecordSource* ProcedureSourceNode::compile(thread_db* tdbb, OptimizerBlk* opt, bool /*innerSubStream*/)
{
	fb_assert(stream <= MAX_UCHAR);
	fb_assert(opt->beds[0] < MAX_STREAMS && opt->beds[0] < MAX_UCHAR); // debug check
	//if (opt->beds[0] >= MAX_STREAMS) // all builds check
	//	ERR_post(Arg::Gds(isc_too_many_contexts));

	opt->beds[++opt->beds[0]] = (UCHAR) stream;

	RecordSource* rsb = generate(tdbb, opt);
	fb_assert(opt->localStreams[0] < MAX_STREAMS && opt->localStreams[0] < MAX_UCHAR);
	opt->localStreams[++opt->localStreams[0]] = stream;

	return rsb;
}

// Compile and optimize a record selection expression into a set of record source blocks (rsb's).
ProcedureScan* ProcedureSourceNode::generate(thread_db* tdbb, OptimizerBlk* opt)
{
	SET_TDBB(tdbb);

	jrd_prc* const proc = MET_lookup_procedure_id(tdbb, procedure, false, false, 0);

	CompilerScratch* const csb = opt->opt_csb;
	CompilerScratch::csb_repeat* const csbTail = &csb->csb_rpt[stream];
	const string alias = OPT_make_alias(tdbb, csb, csbTail);

	return FB_NEW(*tdbb->getDefaultPool()) ProcedureScan(csb, alias, stream, proc,
		sourceList, targetList, in_msg);
}

bool ProcedureSourceNode::computable(CompilerScratch* csb, SSHORT stream, bool idx_use,
	bool allowOnlyCurrentStream, ValueExprNode* /*value*/)
{
	if (sourceList && !sourceList->computable(csb, stream, idx_use, allowOnlyCurrentStream))
		return false;

	if (targetList && !targetList->computable(csb, stream, idx_use, allowOnlyCurrentStream))
		return false;

	return true;
}

void ProcedureSourceNode::findDependentFromStreams(const OptimizerRetrieval* optRet,
	SortedStreamList* streamList)
{
	if (sourceList)
		sourceList->findDependentFromStreams(optRet, streamList);

	if (targetList)
		targetList->findDependentFromStreams(optRet, streamList);
}

bool ProcedureSourceNode::jrdStreamFinder(CompilerScratch* /*csb*/, UCHAR /*findStream*/)
{
	// ASF: We used to visit nodes that were not handled appropriate. This is
	// equivalent with the legacy code.
	return sourceList && targetList;
}

void ProcedureSourceNode::jrdStreamsCollector(SortedArray<int>& streamList)
{
	if (sourceList)
		sourceList->jrdStreamsCollector(streamList);

	if (targetList)
		targetList->jrdStreamsCollector(streamList);
}


//--------------------


// Parse an aggregate reference.
AggregateSourceNode* AggregateSourceNode::parse(thread_db* tdbb, CompilerScratch* csb)
{
	SET_TDBB(tdbb);

	AggregateSourceNode* node = FB_NEW(*tdbb->getDefaultPool()) AggregateSourceNode(
		*tdbb->getDefaultPool());

	node->stream = PAR_context(csb, NULL);
	fb_assert(node->stream <= MAX_STREAMS);
	node->rse = PAR_rse(tdbb, csb);
	node->group = PAR_sort(tdbb, csb, blr_group_by, true);
	node->map = parseMap(tdbb, csb, node->stream);

	return node;
}

bool AggregateSourceNode::dsqlAggregateFinder(AggregateFinder& visitor)
{
	return !visitor.ignoreSubSelects && visitor.visit(&dsqlRse);
}

bool AggregateSourceNode::dsqlAggregate2Finder(Aggregate2Finder& visitor)
{
	// Pass only dsqlGroup.
	return visitor.visit(&dsqlGroup);
}

bool AggregateSourceNode::dsqlInvalidReferenceFinder(InvalidReferenceFinder& visitor)
{
	return visitor.visit(&dsqlRse);
}

bool AggregateSourceNode::dsqlSubSelectFinder(SubSelectFinder& visitor)
{
	return false;
}

bool AggregateSourceNode::dsqlFieldFinder(FieldFinder& visitor)
{
	// Pass only dsqlGroup.
	return visitor.visit(&dsqlGroup);
}

bool AggregateSourceNode::dsqlFieldRemapper(FieldRemapper& visitor)
{
	visitor.visit(&dsqlRse);
	return false;
}

bool AggregateSourceNode::dsqlMatch(const ExprNode* other, bool ignoreMapCast) const
{
	const AggregateSourceNode* o = other->as<AggregateSourceNode>();

	return o && dsqlContext == o->dsqlContext &&
		PASS1_node_match(dsqlGroup, o->dsqlGroup, ignoreMapCast) &&
		PASS1_node_match(dsqlRse, o->dsqlRse, ignoreMapCast);
}

void AggregateSourceNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar((dsqlWindow ? blr_window : blr_aggregate));

	if (!dsqlWindow)
		GEN_stuff_context(dsqlScratch, dsqlContext);

	GEN_rse(dsqlScratch, dsqlRse);

	// Handle PARTITION BY and GROUP BY clause

	if (dsqlWindow)
	{
		fb_assert(dsqlContext->ctx_win_maps.hasData());
		dsqlScratch->appendUChar(dsqlContext->ctx_win_maps.getCount());	// number of windows

		for (Array<PartitionMap*>::iterator i = dsqlContext->ctx_win_maps.begin();
			 i != dsqlContext->ctx_win_maps.end();
			 ++i)
		{
			dsqlScratch->appendUChar(blr_partition_by);
			dsql_nod* partition = (*i)->partition;
			dsql_nod* partitionRemapped = (*i)->partitionRemapped;
			dsql_nod* order = (*i)->order;

			dsqlScratch->appendUChar((*i)->context);

			if (partition)
			{
				dsqlScratch->appendUChar(partition->nod_count);	// partition by expression count

				dsql_nod** ptr = partition->nod_arg;
				for (const dsql_nod* const* end = ptr + partition->nod_count; ptr < end; ++ptr)
					GEN_expr(dsqlScratch, *ptr);

				ptr = partitionRemapped->nod_arg;
				for (const dsql_nod* const* end = ptr + partitionRemapped->nod_count; ptr < end; ++ptr)
					GEN_expr(dsqlScratch, *ptr);
			}
			else
				dsqlScratch->appendUChar(0);	// partition by expression count

			if (order)
				GEN_sort(dsqlScratch, order);
			else
			{
				dsqlScratch->appendUChar(blr_sort);
				dsqlScratch->appendUChar(0);
			}

			genMap(dsqlScratch, (*i)->map);
		}
	}
	else
	{
		dsqlScratch->appendUChar(blr_group_by);

		dsql_nod* list = dsqlGroup;

		if (list != NULL)
		{
			dsqlScratch->appendUChar(list->nod_count);
			dsql_nod** ptr = list->nod_arg;

			for (const dsql_nod* const* end = ptr + list->nod_count; ptr < end; ptr++)
				GEN_expr(dsqlScratch, *ptr);
		}
		else
			dsqlScratch->appendUChar(0);

		genMap(dsqlScratch, dsqlContext->ctx_map);
	}
}

// Generate a value map for a record selection expression.
void AggregateSourceNode::genMap(DsqlCompilerScratch* dsqlScratch, dsql_map* map)
{
	USHORT count = 0;

	for (dsql_map* temp = map; temp; temp = temp->map_next)
		++count;

	dsqlScratch->appendUChar(blr_map);
	dsqlScratch->appendUShort(count);

	for (dsql_map* temp = map; temp; temp = temp->map_next)
	{
		dsqlScratch->appendUShort(temp->map_position);
		GEN_expr(dsqlScratch, temp->map_node);
	}
}

AggregateSourceNode* AggregateSourceNode::copy(thread_db* tdbb, NodeCopier& copier)
{
	if (!copier.remap)
		BUGCHECK(221);	// msg 221 (CMP) copy: cannot remap

	AggregateSourceNode* newSource = FB_NEW(*tdbb->getDefaultPool()) AggregateSourceNode(
		*tdbb->getDefaultPool());

	fb_assert(stream <= MAX_STREAMS);
	newSource->stream = copier.csb->nextStream();
	// fb_assert(newSource->stream <= MAX_UCHAR);
	copier.remap[stream] = (UCHAR) newSource->stream;
	CMP_csb_element(copier.csb, newSource->stream);

	copier.csb->csb_rpt[newSource->stream].csb_flags |=
		copier.csb->csb_rpt[stream].csb_flags & csb_no_dbkey;

	newSource->rse = rse->copy(tdbb, copier);
	if (group)
		newSource->group = group->copy(tdbb, copier);
	newSource->map = map->copy(tdbb, copier);

	return newSource;
}

void AggregateSourceNode::ignoreDbKey(thread_db* tdbb, CompilerScratch* csb) const
{
	rse->ignoreDbKey(tdbb, csb);
}

RecordSourceNode* AggregateSourceNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	fb_assert(stream <= MAX_STREAMS);
	csb->csb_rpt[stream].csb_flags |= csb_no_dbkey;
	rse->ignoreDbKey(tdbb, csb);

	doPass1(tdbb, csb, rse.getAddress());
	doPass1(tdbb, csb, map.getAddress());
	doPass1(tdbb, csb, group.getAddress());

	return this;
}

void AggregateSourceNode::pass1Source(thread_db* tdbb, CompilerScratch* csb, RseNode* /*rse*/,
	BoolExprNode** /*boolean*/, RecordSourceNodeStack& stack)
{
	stack.push(this);	// Assume that the source will be used. Push it on the final stream stack.

	fb_assert(stream <= MAX_STREAMS);
	pass1(tdbb, csb);
}

RecordSourceNode* AggregateSourceNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	rse->pass2Rse(tdbb, csb);
	ExprNode::doPass2(tdbb, csb, map.getAddress());
	ExprNode::doPass2(tdbb, csb, group.getAddress());

	fb_assert(stream <= MAX_STREAMS);

	processMap(tdbb, csb, map, &csb->csb_rpt[stream].csb_internal_format);
	csb->csb_rpt[stream].csb_format = csb->csb_rpt[stream].csb_internal_format;

	return this;
}

void AggregateSourceNode::pass2Rse(thread_db* tdbb, CompilerScratch* csb)
{
	fb_assert(stream <= MAX_STREAMS);
	csb->csb_rpt[stream].csb_flags |= csb_active;

	pass2(tdbb, csb);
}

bool AggregateSourceNode::containsStream(USHORT checkStream) const
{
	// for aggregates, check current RseNode, if not found then check
	// the sub-rse

	if (checkStream == stream)
		return true;		// do not mark as variant

	if (rse->containsStream(checkStream))
		return true;		// do not mark as variant

	return false;
}

RecordSource* AggregateSourceNode::compile(thread_db* tdbb, OptimizerBlk* opt, bool /*innerSubStream*/)
{
	fb_assert(stream <= MAX_UCHAR);
	fb_assert(opt->beds[0] < MAX_STREAMS && opt->beds[0] < MAX_UCHAR); // debug check
	//if (opt->beds[0] >= MAX_STREAMS) // all builds check
	//	ERR_post(Arg::Gds(isc_too_many_contexts));

	opt->beds[++opt->beds[0]] = (UCHAR) stream;

	BoolExprNodeStack::const_iterator stackEnd;
	if (opt->parentStack)
		stackEnd = opt->conjunctStack.merge(*opt->parentStack);

	RecordSource* rsb = generate(tdbb, opt, &opt->conjunctStack, stream);

	if (opt->parentStack)
		opt->conjunctStack.split(stackEnd, *opt->parentStack);

	fb_assert(opt->localStreams[0] < MAX_STREAMS && opt->localStreams[0] < MAX_UCHAR);
	opt->localStreams[++opt->localStreams[0]] = stream;

	return rsb;
}

// Generate a RecordSource (Record Source Block) for each aggregate operation.
// Generate an AggregateSort (Aggregate SortedStream Block) for each DISTINCT aggregate.
RecordSource* AggregateSourceNode::generate(thread_db* tdbb, OptimizerBlk* opt,
	BoolExprNodeStack* parentStack, UCHAR shellStream)
{
	SET_TDBB(tdbb);

	CompilerScratch* const csb = opt->opt_csb;
	rse->rse_sorted = group;

	// AB: Try to distribute items from the HAVING CLAUSE to the WHERE CLAUSE.
	// Zip thru stack of booleans looking for fields that belong to shellStream.
	// Those fields are mappings. Mappings that hold a plain field may be used
	// to distribute. Handle the simple cases only.
	BoolExprNodeStack deliverStack;
	genDeliverUnmapped(tdbb, &deliverStack, map, parentStack, shellStream);

	// try to optimize MAX and MIN to use an index; for now, optimize
	// only the simplest case, although it is probably possible
	// to use an index in more complex situations
	NestConst<ValueExprNode>* ptr;
	AggNode* aggNode = NULL;

	if (map->sourceList.getCount() == 1 && (ptr = map->sourceList.begin()) &&
		(aggNode = (*ptr)->as<AggNode>()) &&
		(aggNode->aggInfo.blr == blr_agg_min || aggNode->aggInfo.blr == blr_agg_max))
	{
		// generate a sort block which the optimizer will try to map to an index

		SortNode* aggregate = rse->rse_aggregate =
			FB_NEW(*tdbb->getDefaultPool()) SortNode(*tdbb->getDefaultPool());

		aggregate->expressions.add(aggNode->arg);
		// in the max case, flag the sort as descending
		aggregate->descending.add(aggNode->aggInfo.blr == blr_agg_max);
		// 10-Aug-2004. Nickolay Samofatov - Unneeded nulls seem to be skipped somehow.
		aggregate->nullOrder.add(rse_nulls_default);
	}

	RecordSource* const nextRsb = OPT_compile(tdbb, csb, rse, &deliverStack);

	fb_assert(stream <= MAX_STREAMS);
	fb_assert(stream <= MAX_UCHAR);

	// allocate and optimize the record source block

	AggregatedStream* const rsb = FB_NEW(*tdbb->getDefaultPool()) AggregatedStream(csb, stream,
		(group ? &group->expressions : NULL), map, nextRsb);

	if (rse->rse_aggregate)
	{
		// The rse_aggregate is still set. That means the optimizer
		// was able to match the field to an index, so flag that fact
		// so that it can be handled in EVL_group
		aggNode->indexed = true;
	}

	OPT_gen_aggregate_distincts(tdbb, csb, map);

	return rsb;
}

bool AggregateSourceNode::computable(CompilerScratch* csb, SSHORT stream, bool idx_use,
	bool allowOnlyCurrentStream, ValueExprNode* /*value*/)
{
	rse->rse_sorted = group;
	return rse->computable(csb, stream, idx_use, allowOnlyCurrentStream, NULL);
}

void AggregateSourceNode::findDependentFromStreams(const OptimizerRetrieval* optRet,
	SortedStreamList* streamList)
{
	rse->rse_sorted = group;
	rse->findDependentFromStreams(optRet, streamList);
}


//--------------------


// Parse a union reference.
UnionSourceNode* UnionSourceNode::parse(thread_db* tdbb, CompilerScratch* csb, SSHORT blrOp)
{
	SET_TDBB(tdbb);

	// Make the node, parse the context number, get a stream assigned,
	// and get the number of sub-RseNode's.

	UnionSourceNode* node = FB_NEW(*tdbb->getDefaultPool()) UnionSourceNode(
		*tdbb->getDefaultPool());

	node->recursive = blrOp == blr_recurse;

	node->stream = PAR_context(csb, NULL);
	fb_assert(node->stream <= MAX_STREAMS);

	// assign separate context for mapped record if union is recursive
	USHORT stream2 = node->stream;

	if (node->recursive)
	{
		stream2 = PAR_context(csb, 0);
		node->mapStream = stream2;
	}

	int count = (unsigned int) csb->csb_blr_reader.getByte();

	// Pick up the sub-RseNode's and maps.

	while (--count >= 0)
	{
		node->clauses.push(PAR_rse(tdbb, csb));
		node->maps.push(parseMap(tdbb, csb, stream2));
	}

	return node;
}

UnionSourceNode* UnionSourceNode::copy(thread_db* tdbb, NodeCopier& copier)
{
	if (!copier.remap)
		BUGCHECK(221);		// msg 221 (CMP) copy: cannot remap

	UnionSourceNode* newSource = FB_NEW(*tdbb->getDefaultPool()) UnionSourceNode(
		*tdbb->getDefaultPool());
	newSource->recursive = recursive;

	fb_assert(stream <= MAX_STREAMS);
	newSource->stream = copier.csb->nextStream();
	copier.remap[stream] = (UCHAR) newSource->stream;
	CMP_csb_element(copier.csb, newSource->stream);

	USHORT oldStream = stream;
	USHORT newStream = newSource->stream;

	if (newSource->recursive)
	{
		oldStream = mapStream;
		fb_assert(oldStream <= MAX_STREAMS);
		newStream = copier.csb->nextStream();
		newSource->mapStream = newStream;
		copier.remap[oldStream] = (UCHAR) newStream;
		CMP_csb_element(copier.csb, newStream);
	}

	copier.csb->csb_rpt[newStream].csb_flags |=
		copier.csb->csb_rpt[oldStream].csb_flags & csb_no_dbkey;

	NestConst<RseNode>* ptr = clauses.begin();
	NestConst<MapNode>* ptr2 = maps.begin();

	for (NestConst<RseNode>* const end = clauses.end(); ptr != end; ++ptr, ++ptr2)
	{
		newSource->clauses.add((*ptr)->copy(tdbb, copier));
		newSource->maps.add((*ptr2)->copy(tdbb, copier));
	}

	return newSource;
}

void UnionSourceNode::ignoreDbKey(thread_db* tdbb, CompilerScratch* csb) const
{
	const NestConst<RseNode>* ptr = clauses.begin();

	for (const NestConst<RseNode>* const end = clauses.end(); ptr != end; ++ptr)
		(*ptr)->ignoreDbKey(tdbb, csb);
}

void UnionSourceNode::pass1Source(thread_db* tdbb, CompilerScratch* csb, RseNode* /*rse*/,
	BoolExprNode** /*boolean*/, RecordSourceNodeStack& stack)
{
	stack.push(this);	// Assume that the source will be used. Push it on the final stream stack.

	NestConst<RseNode>* ptr = clauses.begin();
	NestConst<MapNode>* ptr2 = maps.begin();

	for (NestConst<RseNode>* const end = clauses.end(); ptr != end; ++ptr, ++ptr2)
	{
		doPass1(tdbb, csb, ptr->getAddress());
		doPass1(tdbb, csb, ptr2->getAddress());
	}
}

// Process a union clause of a RseNode.
RecordSourceNode* UnionSourceNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	SET_TDBB(tdbb);

	// make up a format block sufficiently large to hold instantiated record

	const USHORT id = getStream();
	Format** format = &csb->csb_rpt[id].csb_internal_format;

	// Process RseNodes and map blocks.

	NestConst<RseNode>* ptr = clauses.begin();
	NestConst<MapNode>* ptr2 = maps.begin();

	for (NestConst<RseNode>* const end = clauses.end(); ptr != end; ++ptr, ++ptr2)
	{
		(*ptr)->pass2Rse(tdbb, csb);
		ExprNode::doPass2(tdbb, csb, ptr2->getAddress());
		processMap(tdbb, csb, *ptr2, format);
		csb->csb_rpt[id].csb_format = *format;
	}

	if (recursive)
		csb->csb_rpt[mapStream].csb_format = *format;

	return this;
}

void UnionSourceNode::pass2Rse(thread_db* tdbb, CompilerScratch* csb)
{
	fb_assert(stream <= MAX_STREAMS);
	csb->csb_rpt[stream].csb_flags |= csb_active;

	pass2(tdbb, csb);
}

bool UnionSourceNode::containsStream(USHORT checkStream) const
{
	// for unions, check current RseNode, if not found then check
	// all sub-rse's

	if (checkStream == stream)
		return true;		// do not mark as variant

	const NestConst<RseNode>* ptr = clauses.begin();

	for (const NestConst<RseNode>* const end = clauses.end(); ptr != end; ++ptr)
	{
		if ((*ptr)->containsStream(checkStream))
			return true;
	}

	return false;
}

RecordSource* UnionSourceNode::compile(thread_db* tdbb, OptimizerBlk* opt, bool /*innerSubStream*/)
{
	fb_assert(stream <= MAX_UCHAR);
	fb_assert(opt->beds[0] < MAX_STREAMS && opt->beds[0] < MAX_UCHAR); // debug check
	//if (opt->beds[0] >= MAX_STREAMS) // all builds check
	//	ERR_post(Arg::Gds(isc_too_many_contexts));

	opt->beds[++opt->beds[0]] = (UCHAR) stream;

	const SSHORT i = (SSHORT) opt->keyStreams[0];
	computeDbKeyStreams(opt->keyStreams);

	BoolExprNodeStack::const_iterator stackEnd;
	if (opt->parentStack)
		stackEnd = opt->conjunctStack.merge(*opt->parentStack);

	RecordSource* rsb = generate(tdbb, opt, opt->keyStreams + i + 1,
		(USHORT) (opt->keyStreams[0] - i), &opt->conjunctStack, stream);

	if (opt->parentStack)
		opt->conjunctStack.split(stackEnd, *opt->parentStack);

	fb_assert(opt->localStreams[0] < MAX_STREAMS && opt->localStreams[0] < MAX_UCHAR);
	opt->localStreams[++opt->localStreams[0]] = stream;

	return rsb;
}

// Generate an union complex.
RecordSource* UnionSourceNode::generate(thread_db* tdbb, OptimizerBlk* opt, UCHAR* streams,
	USHORT nstreams, BoolExprNodeStack* parentStack, UCHAR shellStream)
{
	SET_TDBB(tdbb);

	CompilerScratch* csb = opt->opt_csb;
	HalfStaticArray<RecordSource*, OPT_STATIC_ITEMS> rsbs;

	const SLONG baseImpure = CMP_impure(csb, 0);

	NestConst<RseNode>* ptr = clauses.begin();
	NestConst<MapNode>* ptr2 = maps.begin();

	for (NestConst<RseNode>* const end = clauses.end(); ptr != end; ++ptr, ++ptr2)
	{
		RseNode* rse = *ptr;
		MapNode* map = *ptr2;

		// AB: Try to distribute booleans from the top rse for an UNION to
		// the WHERE clause of every single rse.
		// hvlad: don't do it for recursive unions else they will work wrong !
		BoolExprNodeStack deliverStack;
		if (!recursive)
			genDeliverUnmapped(tdbb, &deliverStack, map, parentStack, shellStream);

		rsbs.add(OPT_compile(tdbb, csb, rse, &deliverStack));

		// hvlad: activate recursive union itself after processing first (non-recursive)
		// member to allow recursive members be optimized
		if (recursive)
			csb->csb_rpt[stream].csb_flags |= csb_active;
	}

	if (recursive)
	{
		fb_assert(rsbs.getCount() == 2 && maps.getCount() == 2);
		// hvlad: save size of inner impure area and context of mapped record
		// for recursive processing later
		return FB_NEW(*tdbb->getDefaultPool()) RecursiveStream(csb, stream, mapStream,
			rsbs[0], rsbs[1], maps[0], maps[1], nstreams, streams, baseImpure);
	}

	return FB_NEW(*tdbb->getDefaultPool()) Union(csb, stream, clauses.getCount(), rsbs.begin(),
		maps.begin(), nstreams, streams);
}

// Identify all of the streams for which a dbkey may need to be carried through a sort.
void UnionSourceNode::computeDbKeyStreams(UCHAR* streams) const
{
	const NestConst<RseNode>* ptr = clauses.begin();

	for (const NestConst<RseNode>* const end = clauses.end(); ptr != end; ++ptr)
		(*ptr)->computeDbKeyStreams(streams);
}

bool UnionSourceNode::computable(CompilerScratch* csb, SSHORT stream, bool idx_use,
	bool allowOnlyCurrentStream, ValueExprNode* /*value*/)
{
	NestConst<RseNode>* ptr = clauses.begin();

	for (NestConst<RseNode>* const end = clauses.end(); ptr != end; ++ptr)
	{
		if (!(*ptr)->computable(csb, stream, idx_use, allowOnlyCurrentStream, NULL))
			return false;
	}

	return true;
}

void UnionSourceNode::findDependentFromStreams(const OptimizerRetrieval* optRet,
	SortedStreamList* streamList)
{
	NestConst<RseNode>* ptr = clauses.begin();

	for (NestConst<RseNode>* const end = clauses.end(); ptr != end; ++ptr)
		(*ptr)->findDependentFromStreams(optRet, streamList);
}


//--------------------


// Parse a window reference.
WindowSourceNode* WindowSourceNode::parse(thread_db* tdbb, CompilerScratch* csb)
{
	SET_TDBB(tdbb);

	WindowSourceNode* node = FB_NEW(*tdbb->getDefaultPool()) WindowSourceNode(
		*tdbb->getDefaultPool());

	node->rse = PAR_rse(tdbb, csb);

	unsigned partitionCount = csb->csb_blr_reader.getByte();

	for (unsigned i = 0; i < partitionCount; ++i)
		node->parsePartitionBy(tdbb, csb);

	return node;
}

// Parse PARTITION BY subclauses of window functions.
void WindowSourceNode::parsePartitionBy(thread_db* tdbb, CompilerScratch* csb)
{
	SET_TDBB(tdbb);

	if (csb->csb_blr_reader.getByte() != blr_partition_by)
		PAR_syntax_error(csb, "blr_partition_by");

	SSHORT context;
	Partition& partition = partitions.add();
	partition.stream = PAR_context(csb, &context);

	const UCHAR count = csb->csb_blr_reader.getByte();

	if (count != 0)
	{
		partition.group = PAR_sort_internal(tdbb, csb, blr_partition_by, count);
		partition.regroup = PAR_sort_internal(tdbb, csb, blr_partition_by, count);
	}

	partition.order = PAR_sort(tdbb, csb, blr_sort, true);
	partition.map = parseMap(tdbb, csb, partition.stream);
}

WindowSourceNode* WindowSourceNode::copy(thread_db* tdbb, NodeCopier& copier)
{
	if (!copier.remap)
		BUGCHECK(221);		// msg 221 (CMP) copy: cannot remap

	WindowSourceNode* newSource = FB_NEW(*tdbb->getDefaultPool()) WindowSourceNode(
		*tdbb->getDefaultPool());

	newSource->rse = rse->copy(tdbb, copier);

	for (ObjectsArray<Partition>::iterator inputPartition = partitions.begin();
		 inputPartition != partitions.end();
		 ++inputPartition)
	{
		fb_assert(inputPartition->stream <= MAX_STREAMS);

		Partition& copyPartition = newSource->partitions.add();

		copyPartition.stream = copier.csb->nextStream();
		// fb_assert(copyPartition.stream <= MAX_UCHAR);

		copier.remap[inputPartition->stream] = (UCHAR) copyPartition.stream;
		CMP_csb_element(copier.csb, copyPartition.stream);

		if (inputPartition->group)
			copyPartition.group = inputPartition->group->copy(tdbb, copier);
		if (inputPartition->regroup)
			copyPartition.regroup = inputPartition->regroup->copy(tdbb, copier);
		if (inputPartition->order)
			copyPartition.order = inputPartition->order->copy(tdbb, copier);
		copyPartition.map = inputPartition->map->copy(tdbb, copier);
	}

	return newSource;
}

void WindowSourceNode::ignoreDbKey(thread_db* tdbb, CompilerScratch* csb) const
{
	rse->ignoreDbKey(tdbb, csb);
}

RecordSourceNode* WindowSourceNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	for (ObjectsArray<Partition>::iterator partition = partitions.begin();
		 partition != partitions.end();
		 ++partition)
	{
		fb_assert(partition->stream <= MAX_STREAMS);
		csb->csb_rpt[partition->stream].csb_flags |= csb_no_dbkey;
	}

	rse->ignoreDbKey(tdbb, csb);
	doPass1(tdbb, csb, rse.getAddress());

	for (ObjectsArray<Partition>::iterator partition = partitions.begin();
		 partition != partitions.end();
		 ++partition)
	{
		doPass1(tdbb, csb, partition->group.getAddress());
		doPass1(tdbb, csb, partition->regroup.getAddress());
		doPass1(tdbb, csb, partition->order.getAddress());
		doPass1(tdbb, csb, partition->map.getAddress());
	}

	return this;
}

void WindowSourceNode::pass1Source(thread_db* tdbb, CompilerScratch* csb, RseNode* /*rse*/,
	BoolExprNode** /*boolean*/, RecordSourceNodeStack& stack)
{
	stack.push(this);	// Assume that the source will be used. Push it on the final stream stack.

	pass1(tdbb, csb);
}

RecordSourceNode* WindowSourceNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	rse->pass2Rse(tdbb, csb);

	for (ObjectsArray<Partition>::iterator partition = partitions.begin();
		 partition != partitions.end();
		 ++partition)
	{
		ExprNode::doPass2(tdbb, csb, partition->map.getAddress());
		ExprNode::doPass2(tdbb, csb, partition->group.getAddress());
		ExprNode::doPass2(tdbb, csb, partition->order.getAddress());

		fb_assert(partition->stream <= MAX_STREAMS);

		processMap(tdbb, csb, partition->map, &csb->csb_rpt[partition->stream].csb_internal_format);
		csb->csb_rpt[partition->stream].csb_format =
			csb->csb_rpt[partition->stream].csb_internal_format;
	}

	for (ObjectsArray<Partition>::iterator partition = partitions.begin();
		 partition != partitions.end();
		 ++partition)
	{
		ExprNode::doPass2(tdbb, csb, partition->regroup.getAddress());
	}

	return this;
}

void WindowSourceNode::pass2Rse(thread_db* tdbb, CompilerScratch* csb)
{
	pass2(tdbb, csb);

	for (ObjectsArray<Partition>::iterator partition = partitions.begin();
		 partition != partitions.end();
		 ++partition)
	{
		csb->csb_rpt[partition->stream].csb_flags |= csb_active;
	}
}

bool WindowSourceNode::containsStream(USHORT checkStream) const
{
	for (ObjectsArray<Partition>::const_iterator partition = partitions.begin();
		 partition != partitions.end();
		 ++partition)
	{
		if (checkStream == partition->stream)
			return true;		// do not mark as variant
	}

	if (rse->containsStream(checkStream))
		return true;		// do not mark as variant

	return false;
}

RecordSource* WindowSourceNode::compile(thread_db* tdbb, OptimizerBlk* opt, bool /*innerSubStream*/)
{
	for (ObjectsArray<Partition>::iterator partition = partitions.begin();
		 partition != partitions.end();
		 ++partition)
	{
		fb_assert(partition->stream <= MAX_UCHAR);
		fb_assert(opt->beds[0] < MAX_STREAMS && opt->beds[0] < MAX_UCHAR); // debug check
		//if (opt->beds[0] >= MAX_STREAMS) // all builds check
		//	ERR_post(Arg::Gds(isc_too_many_contexts));

		opt->beds[++opt->beds[0]] = (UCHAR) partition->stream;
	}

	BoolExprNodeStack deliverStack;

	RecordSource* rsb = FB_NEW(*tdbb->getDefaultPool()) WindowedStream(opt->opt_csb, partitions,
		OPT_compile(tdbb, opt->opt_csb, rse, &deliverStack));

	StreamsArray rsbStreams;
	rsb->findUsedStreams(rsbStreams);

	for (StreamsArray::iterator i = rsbStreams.begin(); i != rsbStreams.end(); ++i)
	{
		fb_assert(opt->localStreams[0] < MAX_STREAMS && opt->localStreams[0] < MAX_UCHAR);
		opt->localStreams[++opt->localStreams[0]] = *i;
	}

	return rsb;
}

bool WindowSourceNode::computable(CompilerScratch* csb, SSHORT stream, bool idx_use,
	bool allowOnlyCurrentStream, ValueExprNode* /*value*/)
{
	return rse->computable(csb, stream, idx_use, allowOnlyCurrentStream, NULL);
}

void WindowSourceNode::getStreams(StreamsArray& list) const
{
	for (ObjectsArray<Partition>::const_iterator partition = partitions.begin();
		 partition != partitions.end();
		 ++partition)
	{
		list.add(partition->stream);
	}
}

void WindowSourceNode::findDependentFromStreams(const OptimizerRetrieval* optRet,
	SortedStreamList* streamList)
{
	rse->findDependentFromStreams(optRet, streamList);
}


//--------------------


bool RseNode::dsqlAggregateFinder(AggregateFinder& visitor)
{
	AutoSetRestore<USHORT> autoValidateExpr(&visitor.currentLevel, visitor.currentLevel + 1);
	return visitor.visit(&dsqlStreams) | visitor.visit(&dsqlWhere) | visitor.visit(&dsqlSelectList);
}

bool RseNode::dsqlAggregate2Finder(Aggregate2Finder& visitor)
{
	AutoSetRestore<bool> autoCurrentScopeLevelEqual(&visitor.currentScopeLevelEqual, false);
	// Pass dsqlWhere and dsqlSelectList
	return visitor.visit(&dsqlWhere) | visitor.visit(&dsqlSelectList);
}

bool RseNode::dsqlInvalidReferenceFinder(InvalidReferenceFinder& visitor)
{
	return false;
}

bool RseNode::dsqlSubSelectFinder(SubSelectFinder& visitor)
{
	return true;
}

bool RseNode::dsqlFieldFinder(FieldFinder& visitor)
{
	// Pass dsqlWhere and dsqlSelectList
	return visitor.visit(&dsqlWhere) | visitor.visit(&dsqlSelectList);
}

bool RseNode::dsqlFieldRemapper(FieldRemapper& visitor)
{
	AutoSetRestore<USHORT> autoCurrentLevel(&visitor.currentLevel, visitor.currentLevel + 1);

	visitor.visit(&dsqlStreams);
	visitor.visit(&dsqlWhere);
	visitor.visit(&dsqlSelectList);
	visitor.visit(&dsqlOrder);

	return false;
}

bool RseNode::dsqlMatch(const ExprNode* other, bool ignoreMapCast) const
{
	const RseNode* o = other->as<RseNode>();

	if (!o)
		return false;

	fb_assert(dsqlContext && o->dsqlContext);

	return dsqlContext == o->dsqlContext;
}

// Make up join node and mark relations as "possibly NULL" if they are in outer joins (inOuterJoin).
RseNode* RseNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	// Set up an empty context to process the joins

	DsqlContextStack* const base_context = dsqlScratch->context;
	DsqlContextStack temp;
	dsqlScratch->context = &temp;

	RseNode* node = FB_NEW(getPool()) RseNode(getPool());
	node->dsqlExplicitJoin = dsqlExplicitJoin;
	node->rse_jointype = rse_jointype;
	node->dsqlStreams = MAKE_node(Dsql::nod_list, dsqlFrom->nod_count);

	switch (rse_jointype)
	{
		case blr_inner:
			node->dsqlStreams->nod_arg[0] = PASS1_node(dsqlScratch, dsqlFrom->nod_arg[0]);
			node->dsqlStreams->nod_arg[1] = PASS1_node(dsqlScratch, dsqlFrom->nod_arg[1]);
			break;

		case blr_left:
			node->dsqlStreams->nod_arg[0] = PASS1_node(dsqlScratch, dsqlFrom->nod_arg[0]);
			++dsqlScratch->inOuterJoin;
			node->dsqlStreams->nod_arg[1] = PASS1_node(dsqlScratch, dsqlFrom->nod_arg[1]);
			--dsqlScratch->inOuterJoin;
			break;

		case blr_right:
			++dsqlScratch->inOuterJoin;
			node->dsqlStreams->nod_arg[0] = PASS1_node(dsqlScratch, dsqlFrom->nod_arg[0]);
			--dsqlScratch->inOuterJoin;
			node->dsqlStreams->nod_arg[1] = PASS1_node(dsqlScratch, dsqlFrom->nod_arg[1]);
			break;

		case blr_full:
			++dsqlScratch->inOuterJoin;
			node->dsqlStreams->nod_arg[0] = PASS1_node(dsqlScratch, dsqlFrom->nod_arg[0]);
			node->dsqlStreams->nod_arg[1] = PASS1_node(dsqlScratch, dsqlFrom->nod_arg[1]);
			--dsqlScratch->inOuterJoin;
			break;

		default:
			fb_assert(false);
			break;
	}

	dsql_nod* boolean = dsqlWhere;

	if (boolean && (boolean->nod_type == Dsql::nod_flag || boolean->nod_type == Dsql::nod_list))
	{
		if (dsqlScratch->clientDialect < SQL_DIALECT_V6)
		{
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-901) <<
					  Arg::Gds(isc_dsql_unsupp_feature_dialect) << Arg::Num(dsqlScratch->clientDialect));
		}

		DsqlNodStack leftStack, rightStack;

		if (boolean->nod_type == Dsql::nod_flag)	// NATURAL JOIN
		{
			StrArray leftNames(dsqlScratch->getPool());
			DsqlNodStack matched;

			PASS1_expand_select_node(dsqlScratch, node->dsqlStreams->nod_arg[0], leftStack, true);
			PASS1_expand_select_node(dsqlScratch, node->dsqlStreams->nod_arg[1], rightStack, true);

			// verify columns that exist in both sides
			for (int i = 0; i < 2; ++i)
			{
				for (DsqlNodStack::const_iterator j(i == 0 ? leftStack : rightStack); j.hasData(); ++j)
				{
					const TEXT* name = NULL;
					dsql_nod* item = j.object();
					FieldNode* fieldNode;
					DerivedFieldNode* derivedField;

					switch (item->nod_type)
					{
						case Dsql::nod_alias:
							name = reinterpret_cast<dsql_str*>(item->nod_arg[Dsql::e_alias_alias])->str_data;
							break;

						default:
							if ((fieldNode = ExprNode::as<FieldNode>(item)))
								name = fieldNode->dsqlField->fld_name.c_str();
							else if ((derivedField = ExprNode::as<DerivedFieldNode>(item)))
								name = derivedField->name.c_str();
							break;
					}

					if (name)
					{
						if (i == 0)	// left
							leftNames.add(name);
						else	// right
						{
							if (leftNames.exist(name))
								matched.push(MAKE_field_name(name));
						}
					}
				}
			}

			if (matched.isEmpty())
			{
				// There is no match. Transform to CROSS JOIN.
				node->rse_jointype = blr_inner;
				boolean = NULL;
			}
			else
				boolean = MAKE_list(matched);	// Transform to USING
		}

		if (boolean)	// JOIN ... USING
		{
			fb_assert(boolean->nod_type == Dsql::nod_list);

			dsql_nod* newBoolean = NULL;
			StrArray usedColumns(dsqlScratch->getPool());

			for (int i = 0; i < boolean->nod_count; ++i)
			{
				dsql_nod* field = boolean->nod_arg[i];
				dsql_str* fldName = reinterpret_cast<dsql_str*>(field->nod_arg[Dsql::e_fln_name]);

				// verify if the column was already used
				size_t pos;
				if (usedColumns.find(fldName->str_data, pos))
				{
					ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
							  Arg::Gds(isc_dsql_col_more_than_once_using) << Arg::Str(fldName->str_data));
				}
				else
					usedColumns.insert(pos, fldName->str_data);

				dsql_ctx* leftCtx = NULL;
				dsql_ctx* rightCtx = NULL;

				// clear the stacks for the next pass
				leftStack.clear();
				rightStack.clear();

				// get the column names from both sides
				PASS1_expand_select_node(dsqlScratch, node->dsqlStreams->nod_arg[0], leftStack, true);
				PASS1_expand_select_node(dsqlScratch, node->dsqlStreams->nod_arg[1], rightStack, true);

				// create the boolean

				ComparativeBoolNode* eqlNode = FB_NEW(getPool()) ComparativeBoolNode(getPool(), blr_eql);
				eqlNode->dsqlArg1 = resolveUsingField(dsqlScratch, fldName, leftStack, field,
					"left", leftCtx);
				eqlNode->dsqlArg2 = resolveUsingField(dsqlScratch, fldName, rightStack, field,
					"right", rightCtx);

				dsql_nod* eqlNod = MAKE_node(Dsql::nod_class_exprnode, 1);
				eqlNod->nod_arg[0] = reinterpret_cast<dsql_nod*>(eqlNode);

				fb_assert(leftCtx);
				fb_assert(rightCtx);

				// We should hide the (unqualified) column in one side
				ImplicitJoin* impJoinLeft;
				if (!leftCtx->ctx_imp_join.get(fldName->str_data, impJoinLeft))
				{
					impJoinLeft = FB_NEW(dsqlScratch->getPool()) ImplicitJoin();
					impJoinLeft->value = eqlNode->dsqlArg1;
					impJoinLeft->visibleInContext = leftCtx;
				}
				else
					fb_assert(impJoinLeft->visibleInContext == leftCtx);

				ImplicitJoin* impJoinRight;
				if (!rightCtx->ctx_imp_join.get(fldName->str_data, impJoinRight))
				{
					impJoinRight = FB_NEW(dsqlScratch->getPool()) ImplicitJoin();
					impJoinRight->value = eqlNode->dsqlArg2;
				}
				else
					fb_assert(impJoinRight->visibleInContext == rightCtx);

				// create the COALESCE
				DsqlNodStack stack;

				dsql_nod* temp = impJoinLeft->value;
				if (temp->nod_type == Dsql::nod_alias)
					temp = temp->nod_arg[Dsql::e_alias_value];

				{	// scope
					PsqlChanger changer(dsqlScratch, false);

					if (temp->nod_type == Dsql::nod_coalesce)
					{
						PASS1_put_args_on_stack(dsqlScratch, temp->nod_arg[0], stack);
						PASS1_put_args_on_stack(dsqlScratch, temp->nod_arg[1], stack);
					}
					else
						PASS1_put_args_on_stack(dsqlScratch, temp, stack);

					if ((temp = impJoinRight->value)->nod_type == Dsql::nod_alias)
						temp = temp->nod_arg[Dsql::e_alias_value];

					if (temp->nod_type == Dsql::nod_coalesce)
					{
						PASS1_put_args_on_stack(dsqlScratch, temp->nod_arg[0], stack);
						PASS1_put_args_on_stack(dsqlScratch, temp->nod_arg[1], stack);
					}
					else
						PASS1_put_args_on_stack(dsqlScratch, temp, stack);
				}

				dsql_nod* coalesce = MAKE_node(Dsql::nod_coalesce, 2);
				coalesce->nod_arg[0] = stack.pop();
				coalesce->nod_arg[1] = MAKE_list(stack);

				impJoinLeft->value = MAKE_node(Dsql::nod_alias, Dsql::e_alias_count);
				impJoinLeft->value->nod_arg[Dsql::e_alias_value] = coalesce;
				impJoinLeft->value->nod_arg[Dsql::e_alias_alias] = reinterpret_cast<dsql_nod*>(fldName);
				impJoinLeft->value->nod_arg[Dsql::e_alias_imp_join] = reinterpret_cast<dsql_nod*>(impJoinLeft);

				impJoinRight->visibleInContext = NULL;

				// both sides should refer to the same ImplicitJoin
				leftCtx->ctx_imp_join.put(fldName->str_data, impJoinLeft);
				rightCtx->ctx_imp_join.put(fldName->str_data, impJoinLeft);

				newBoolean = PASS1_compose(newBoolean, eqlNod, blr_and);
			}

			boolean = newBoolean;
		}
	}

	node->dsqlWhere = PASS1_node(dsqlScratch, boolean);

	// Merge the newly created contexts with the original ones

	while (temp.hasData())
		base_context->push(temp.pop());

	dsqlScratch->context = base_context;

	return node;
}

RseNode* RseNode::copy(thread_db* tdbb, NodeCopier& copier)
{
	RseNode* newSource = FB_NEW(*tdbb->getDefaultPool()) RseNode(*tdbb->getDefaultPool());

	NestConst<RecordSourceNode>* ptr = rse_relations.begin();

	for (NestConst<RecordSourceNode>* const end = rse_relations.end(); ptr != end; ++ptr)
		newSource->rse_relations.add((*ptr)->copy(tdbb, copier));

	newSource->flags = flags;
	newSource->rse_jointype = rse_jointype;
	newSource->rse_first = copier.copy(tdbb, rse_first);
	newSource->rse_skip = copier.copy(tdbb, rse_skip);

	if (rse_boolean)
		newSource->rse_boolean = copier.copy(tdbb, rse_boolean);

	if (rse_sorted)
		newSource->rse_sorted = rse_sorted->copy(tdbb, copier);

	if (rse_projection)
		newSource->rse_projection = rse_projection->copy(tdbb, copier);

	return newSource;
}

// For each relation or aggregate in the RseNode, mark it as not having a dbkey.
void RseNode::ignoreDbKey(thread_db* tdbb, CompilerScratch* csb) const
{
	const NestConst<RecordSourceNode>* ptr = rse_relations.begin();

	for (const NestConst<RecordSourceNode>* const end = rse_relations.end(); ptr != end; ++ptr)
		(*ptr)->ignoreDbKey(tdbb, csb);
}

// Process a record select expression during pass 1 of compilation.
// Mostly this involves expanding views.
RseNode* RseNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	SET_TDBB(tdbb);

	// for scoping purposes, maintain a stack of RseNode's which are
	// currently being parsed; if there are none on the stack as
	// yet, mark the RseNode as variant to make sure that statement-
	// level aggregates are not treated as invariants -- bug #6535

	bool topLevelRse = true;

	for (RseOrExprNode* node = csb->csb_current_nodes.begin();
		 node != csb->csb_current_nodes.end(); ++node)
	{
		if (node->rseNode)
		{
			topLevelRse = false;
			break;
		}
	}

	if (topLevelRse)
		flags |= FLAG_VARIANT;

	csb->csb_current_nodes.push(this);

	RecordSourceNodeStack stack;
	BoolExprNode* boolean = NULL;
	SortNode* sort = rse_sorted;
	SortNode* project = rse_projection;
	ValueExprNode* first = rse_first;
	ValueExprNode* skip = rse_skip;
	PlanNode* plan = rse_plan;

	// zip thru RseNode expanding views and inner joins
	NestConst<RecordSourceNode>* arg = rse_relations.begin();
	for (const NestConst<RecordSourceNode>* const end = rse_relations.end(); arg != end; ++arg)
		processSource(tdbb, csb, this, *arg, &boolean, stack);

	// Now, rebuild the RseNode block.

	rse_relations.resize(stack.getCount());
	arg = rse_relations.end();

	while (stack.hasData())
		*--arg = stack.pop();

	AutoSetRestore<bool> autoValidateExpr(&csb->csb_validate_expr, false);

	// finish of by processing other clauses

	if (first)
	{
		doPass1(tdbb, csb, &first);
		rse_first = first;
	}

	if (skip)
	{
		doPass1(tdbb, csb, &skip);
		rse_skip = skip;
	}

	if (boolean)
	{
		if (rse_boolean)
		{
			BinaryBoolNode* andNode = FB_NEW(csb->csb_pool) BinaryBoolNode(csb->csb_pool, blr_and);
			andNode->arg1 = boolean;
			andNode->arg2 = rse_boolean;

			doPass1(tdbb, csb, andNode->arg2.getAddress());

			rse_boolean = andNode;
		}
		else
			rse_boolean = boolean;
	}
	else if (rse_boolean)
		doPass1(tdbb, csb, rse_boolean.getAddress());

	if (sort)
	{
		doPass1(tdbb, csb, &sort);
		rse_sorted = sort;
	}

	if (project)
	{
		doPass1(tdbb, csb, &project);
		rse_projection = project;
	}

	if (plan)
		rse_plan = plan;

	// we are no longer in the scope of this RseNode
	csb->csb_current_nodes.pop();

	return this;
}

void RseNode::pass1Source(thread_db* tdbb, CompilerScratch* csb, RseNode* rse,
	BoolExprNode** boolean, RecordSourceNodeStack& stack)
{
	// in the case of an RseNode, it is possible that a new RseNode will be generated,
	// so wait to process the source before we push it on the stack (bug 8039)

	// The addition of the JOIN syntax for specifying inner joins causes an
	// RseNode tree to be generated, which is undesirable in the simplest case
	// where we are just trying to inner join more than 2 streams. If possible,
	// try to flatten the tree out before we go any further.

	if (!rse->rse_jointype && !rse_jointype && !rse_sorted && !rse_projection &&
		!rse_first && !rse_skip && !rse_plan)
	{
		NestConst<RecordSourceNode>* arg = rse_relations.begin();
		for (const NestConst<RecordSourceNode>* const end = rse_relations.end(); arg != end; ++arg)
			processSource(tdbb, csb, rse, *arg, boolean, stack);

		// fold in the boolean for this inner join with the one for the parent

		if (rse_boolean)
		{
			BoolExprNode* node = rse_boolean;
			doPass1(tdbb, csb, &node);

			if (*boolean)
			{
				BinaryBoolNode* andNode = FB_NEW(csb->csb_pool) BinaryBoolNode(csb->csb_pool, blr_and);
				andNode->arg1 = node;
				andNode->arg2 = *boolean;

				*boolean = andNode;
			}
			else
				*boolean = node;
		}

		return;
	}

	pass1(tdbb, csb);
	stack.push(this);
}

// Perform the first half of record selection expression compilation.
// The actual optimization is done in "post_rse".
void RseNode::pass2Rse(thread_db* tdbb, CompilerScratch* csb)
{
	SET_TDBB(tdbb);

	// Maintain stack of RSEe for scoping purposes
	csb->csb_current_nodes.push(this);

	if (rse_first)
		ExprNode::doPass2(tdbb, csb, rse_first.getAddress());

	if (rse_skip)
	    ExprNode::doPass2(tdbb, csb, rse_skip.getAddress());

	NestConst<RecordSourceNode>* ptr = rse_relations.begin();

	for (const NestConst<RecordSourceNode>* const end = rse_relations.end(); ptr != end; ++ptr)
		(*ptr)->pass2Rse(tdbb, csb);

	ExprNode::doPass2(tdbb, csb, rse_boolean.getAddress());
	ExprNode::doPass2(tdbb, csb, rse_sorted.getAddress());
	ExprNode::doPass2(tdbb, csb, rse_projection.getAddress());

	// If the user has submitted a plan for this RseNode, check it for correctness.

	if (rse_plan)
	{
		planSet(csb, rse_plan);
		planCheck(csb);
	}

	csb->csb_current_nodes.pop();
}

// Return true if stream is contained in the specified RseNode.
bool RseNode::containsStream(USHORT checkStream) const
{
	// Look through all relation nodes in this RseNode to see
	// if the field references this instance of the relation.

	const NestConst<RecordSourceNode>* ptr = rse_relations.begin();

	for (const NestConst<RecordSourceNode>* const end = rse_relations.end(); ptr != end; ++ptr)
	{
		const RecordSourceNode* sub = *ptr;

		if (sub->containsStream(checkStream))
			return true;		// do not mark as variant
	}

	return false;
}

RecordSource* RseNode::compile(thread_db* tdbb, OptimizerBlk* opt, bool innerSubStream)
{
	// for nodes which are not relations, generate an rsb to
	// represent that work has to be done to retrieve them;
	// find all the substreams involved and compile them as well

	computeRseStreams(opt->opt_csb, opt->beds);
	computeRseStreams(opt->opt_csb, opt->localStreams);
	computeDbKeyStreams(opt->keyStreams);

	RecordSource* rsb;

	// pass RseNode boolean only to inner substreams because join condition
	// should never exclude records from outer substreams
	if (opt->rse->rse_jointype == blr_inner || (opt->rse->rse_jointype == blr_left && innerSubStream))
	{
		// AB: For an (X LEFT JOIN Y) mark the outer-streams (X) as
		// active because the inner-streams (Y) are always "dependent"
		// on the outer-streams. So that index retrieval nodes could be made.
		// For an INNER JOIN mark previous generated RecordSource's as active.
		if (opt->rse->rse_jointype == blr_left)
		{
			for (StreamsArray::iterator i = opt->outerStreams.begin(); i != opt->outerStreams.end(); ++i)
				opt->opt_csb->csb_rpt[*i].csb_flags |= csb_active;
		}

		//const BoolExprNodeStack::iterator stackSavepoint(opt->conjunctStack);
		BoolExprNodeStack::const_iterator stackEnd;
		BoolExprNodeStack deliverStack;

		if (opt->rse->rse_jointype != blr_inner)
		{
			// Make list of nodes that can be delivered to an outer-stream.
			// In fact these are all nodes except when a IS NULL comparison is done.
			// Note! Don't forget that this can be burried inside a expression
			// such as "CASE WHEN (FieldX IS NULL) THEN 0 ELSE 1 END = 0"
			BoolExprNodeStack::iterator stackItem;
			if (opt->parentStack)
				stackItem = *opt->parentStack;

			for (; stackItem.hasData(); ++stackItem)
			{
				BoolExprNode* deliverNode = stackItem.object();

				if (!deliverNode->jrdPossibleUnknownFinder())
					deliverStack.push(deliverNode);
			}

			stackEnd = opt->conjunctStack.merge(deliverStack);
		}
		else
		{
			if (opt->parentStack)
				stackEnd = opt->conjunctStack.merge(*opt->parentStack);
		}

		rsb = OPT_compile(tdbb, opt->opt_csb, this, &opt->conjunctStack);

		if (opt->rse->rse_jointype != blr_inner)
		{
			// Remove previously added parent conjuctions from the stack.
			opt->conjunctStack.split(stackEnd, deliverStack);
		}
		else
		{
			if (opt->parentStack)
				opt->conjunctStack.split(stackEnd, *opt->parentStack);
		}

		if (opt->rse->rse_jointype == blr_left)
		{
			for (StreamsArray::iterator i = opt->outerStreams.begin(); i != opt->outerStreams.end(); ++i)
				opt->opt_csb->csb_rpt[*i].csb_flags &= ~csb_active;
		}
	}
	else
		rsb = OPT_compile(tdbb, opt->opt_csb, this, opt->parentStack);

	return rsb;
}

// Identify the streams that make up a RseNode.
void RseNode::computeRseStreams(const CompilerScratch* csb, UCHAR* streams) const
{
	const NestConst<RecordSourceNode>* ptr = rse_relations.begin();

	for (const NestConst<RecordSourceNode>* const end = rse_relations.end(); ptr != end; ++ptr)
	{
		const RecordSourceNode* node = *ptr;

		if (node->type == RseNode::TYPE)
			static_cast<const RseNode*>(node)->computeRseStreams(csb, streams);
		else
		{
			StreamsArray sourceStreams;
			node->getStreams(sourceStreams);

			for (StreamsArray::iterator i = sourceStreams.begin(); i != sourceStreams.end(); ++i)
			{
				fb_assert(streams[0] < MAX_STREAMS && streams[0] < MAX_UCHAR);
				streams[++streams[0]] = (UCHAR) *i;
			}
		}
	}
}

// Check that all streams in the RseNode have a plan specified for them.
// If they are not, there are streams in the RseNode which were not mentioned in the plan.
void RseNode::planCheck(const CompilerScratch* csb) const
{
	// if any streams are not marked with a plan, give an error

	const NestConst<RecordSourceNode>* ptr = rse_relations.begin();
	for (const NestConst<RecordSourceNode>* const end = rse_relations.end(); ptr != end; ++ptr)
	{
		const RecordSourceNode* node = *ptr;

		if (node->type == RelationSourceNode::TYPE)
		{
			const USHORT stream = node->getStream();

			if (!(csb->csb_rpt[stream].csb_plan))
			{
				ERR_post(Arg::Gds(isc_no_stream_plan) <<
					Arg::Str(csb->csb_rpt[stream].csb_relation->rel_name));
			}
		}
		else if (node->type == RseNode::TYPE)
			static_cast<const RseNode*>(node)->planCheck(csb);
	}
}

// Go through the streams in the plan, find the corresponding streams in the RseNode and store the
// plan for that stream. Do it once and only once to make sure there is a one-to-one correspondence
// between streams in the query and streams in the plan.
void RseNode::planSet(CompilerScratch* csb, PlanNode* plan)
{
	if (plan->type == PlanNode::TYPE_JOIN)
	{
		for (NestConst<PlanNode>* ptr = plan->subNodes.begin(), *end = plan->subNodes.end();
			 ptr != end;
			 ++ptr)
		{
			planSet(csb, *ptr);
		}
	}

	if (plan->type != PlanNode::TYPE_RETRIEVE)
		return;

	const jrd_rel* viewRelation = NULL;
	const jrd_rel* planRelation = plan->relationNode->relation;
	const char* planAlias = plan->relationNode->alias.c_str();

	// find the tail for the relation specified in the RseNode

	const USHORT stream = plan->relationNode->getStream();
	CompilerScratch::csb_repeat* tail = &csb->csb_rpt[stream];

	// if the plan references a view, find the real base relation
	// we are interested in by searching the view map
	UCHAR* map = NULL;

	if (tail->csb_map)
	{
		const TEXT* p = planAlias;

		// if the user has specified an alias, skip past it to find the alias
		// for the base table (if multiple aliases are specified)
		if (p && *p &&
			((tail->csb_relation && !strcmpSpace(tail->csb_relation->rel_name.c_str(), p)) ||
			 (tail->csb_alias && !strcmpSpace(tail->csb_alias->c_str(), p))))
		{
			while (*p && *p != ' ')
				p++;

			if (*p == ' ')
				p++;
		}

		// loop through potentially a stack of views to find the appropriate base table
		UCHAR* mapBase;

		while ( (mapBase = tail->csb_map) )
		{
			map = mapBase;
			tail = &csb->csb_rpt[*map];
			viewRelation = tail->csb_relation;

			// if the plan references the view itself, make sure that
			// the view is on a single table; if it is, fix up the plan
			// to point to the base relation

			if (viewRelation->rel_id == planRelation->rel_id)
			{
				if (!mapBase[2])
				{
					map++;
					tail = &csb->csb_rpt[*map];
				}
				else
				{
					// view %s has more than one base relation; use aliases to distinguish
					ERR_post(Arg::Gds(isc_view_alias) << Arg::Str(planRelation->rel_name));
				}

				break;
			}

			viewRelation = NULL;

			// if the user didn't specify an alias (or didn't specify one
			// for this level), check to make sure there is one and only one
			// base relation in the table which matches the plan relation

			if (!*p)
			{
				const jrd_rel* duplicateRelation = NULL;
				UCHAR* duplicateMap = mapBase;

				map = NULL;

				for (duplicateMap++; *duplicateMap; ++duplicateMap)
				{
					CompilerScratch::csb_repeat* duplicateTail = &csb->csb_rpt[*duplicateMap];
					const jrd_rel* relation = duplicateTail->csb_relation;

					if (relation && relation->rel_id == planRelation->rel_id)
					{
						if (duplicateRelation)
						{
							// table %s is referenced twice in view; use an alias to distinguish
							ERR_post(Arg::Gds(isc_duplicate_base_table) <<
								Arg::Str(duplicateRelation->rel_name));
						}
						else
						{
							duplicateRelation = relation;
							map = duplicateMap;
							tail = duplicateTail;
						}
					}
				}

				break;
			}

			// look through all the base relations for a match

			map = mapBase;
			for (map++; *map; map++)
			{
				tail = &csb->csb_rpt[*map];
				const jrd_rel* relation = tail->csb_relation;

				// match the user-supplied alias with the alias supplied
				// with the view definition; failing that, try the base
				// table name itself

				// CVC: I found that "relation" can be NULL, too. This may be an
				// indication of a logic flaw while parsing the user supplied SQL plan
				// and not an oversight here. It's hard to imagine a csb->csb_rpt with
				// a NULL relation. See exe.h for CompilerScratch struct and its inner csb_repeat struct.

				if ((tail->csb_alias && !strcmpSpace(tail->csb_alias->c_str(), p)) ||
					(relation && !strcmpSpace(relation->rel_name.c_str(), p)))
				{
					break;
				}
			}

			// skip past the alias

			while (*p && *p != ' ')
				p++;

			if (*p == ' ')
				p++;

			if (!*map)
			{
				// table %s is referenced in the plan but not the from list
				ERR_post(Arg::Gds(isc_stream_not_found) << Arg::Str(planRelation->rel_name));
			}
		}

		// fix up the relation node to point to the base relation's stream

		if (!map || !*map)
		{
			// table %s is referenced in the plan but not the from list
			ERR_post(Arg::Gds(isc_stream_not_found) << Arg::Str(planRelation->rel_name));
		}

		plan->relationNode->setStream(*map);
	}

	// make some validity checks

	if (!tail->csb_relation)
	{
		// table %s is referenced in the plan but not the from list
		ERR_post(Arg::Gds(isc_stream_not_found) << Arg::Str(planRelation->rel_name));
	}

	if ((tail->csb_relation->rel_id != planRelation->rel_id) && !viewRelation)
	{
		// table %s is referenced in the plan but not the from list
		ERR_post(Arg::Gds(isc_stream_not_found) << Arg::Str(planRelation->rel_name));
	}

	// check if we already have a plan for this stream

	if (tail->csb_plan)
	{
		// table %s is referenced more than once in plan; use aliases to distinguish
		ERR_post(Arg::Gds(isc_stream_twice) << Arg::Str(tail->csb_relation->rel_name));
	}

	tail->csb_plan = plan;
}

// Identify all of the streams for which a dbkey may need to be carried through a sort.
void RseNode::computeDbKeyStreams(UCHAR* streams) const
{
	const NestConst<RecordSourceNode>* ptr = rse_relations.begin();

	for (const NestConst<RecordSourceNode>* const end = rse_relations.end(); ptr != end; ++ptr)
		(*ptr)->computeDbKeyStreams(streams);
}

bool RseNode::computable(CompilerScratch* csb, SSHORT stream, bool idx_use,
	bool allowOnlyCurrentStream, ValueExprNode* value)
{
	if (rse_first && !rse_first->computable(csb, stream, idx_use, allowOnlyCurrentStream))
		return false;

    if (rse_skip && !rse_skip->computable(csb, stream, idx_use, allowOnlyCurrentStream))
        return false;

	const NestConst<RecordSourceNode>* const end = rse_relations.end();
	NestConst<RecordSourceNode>* ptr;

	// Set sub-streams of rse active

	for (ptr = rse_relations.begin(); ptr != end; ++ptr)
	{
		const RecordSourceNode* const node = *ptr;

		StreamsArray streams;
		node->getStreams(streams);

		for (StreamsArray::iterator i = streams.begin(); i != streams.end(); ++i)
			csb->csb_rpt[*i].csb_flags |= (csb_active | csb_sub_stream);
	}

	bool result = true;

	// Check sub-stream
	if ((rse_boolean && !rse_boolean->computable(csb, stream, idx_use, allowOnlyCurrentStream)) ||
	    (rse_sorted && !rse_sorted->computable(csb, stream, idx_use, allowOnlyCurrentStream)) ||
	    (rse_projection && !rse_projection->computable(csb, stream, idx_use, allowOnlyCurrentStream)))
	{
		result = false;
	}

	for (ptr = rse_relations.begin(); ptr != end && result; ++ptr)
	{
		if (!(*ptr)->computable(csb, stream, idx_use, allowOnlyCurrentStream, NULL))
			result = false;
	}

	// Check value expression, if any
	if (result && value && !value->computable(csb, stream, idx_use, allowOnlyCurrentStream))
		result = false;

	// Reset streams inactive

	for (ptr = rse_relations.begin(); ptr != end; ++ptr)
	{
		const RecordSourceNode* const node = *ptr;

		StreamsArray streams;
		node->getStreams(streams);

		for (StreamsArray::iterator i = streams.begin(); i != streams.end(); ++i)
			csb->csb_rpt[*i].csb_flags &= ~(csb_active | csb_sub_stream);
	}

	return result;
}

void RseNode::findDependentFromStreams(const OptimizerRetrieval* optRet,
	SortedStreamList* streamList)
{
	if (rse_first)
		rse_first->findDependentFromStreams(optRet, streamList);

    if (rse_skip)
        rse_skip->findDependentFromStreams(optRet, streamList);

	if (rse_boolean)
		rse_boolean->findDependentFromStreams(optRet, streamList);

	if (rse_sorted)
		rse_sorted->findDependentFromStreams(optRet, streamList);

	if (rse_projection)
		rse_projection->findDependentFromStreams(optRet, streamList);

	NestConst<RecordSourceNode>* ptr;
	const NestConst<RecordSourceNode>* end;

	for (ptr = rse_relations.begin(), end = rse_relations.end(); ptr != end; ++ptr)
		(*ptr)->findDependentFromStreams(optRet, streamList);
}

bool RseNode::jrdStreamFinder(CompilerScratch* csb, UCHAR findStream)
{
	if (rse_first && rse_first->jrdStreamFinder(csb, findStream))
		return true;

	if (rse_skip && rse_skip->jrdStreamFinder(csb, findStream))
		return true;

	if (rse_boolean && rse_boolean->jrdStreamFinder(csb, findStream))
		return true;

	// ASF: The legacy code used to visit rse_sorted and rse_projection. But note that
	// visiting them, the visitor always returns true, because nod_sort is not handled
	// there. So I replaced these lines by the if/return below.
	//
	// if (rse_sorted && rse_sorted->jrdStreamFinder(csb, findStream))
	//     return true;
	//
	// if (rse_projection && rse_projection->jrdStreamFinder(csb, findStream))
	//     return true;

	if (rse_sorted || rse_projection)
		return true;

	return false;
}

void RseNode::jrdStreamsCollector(SortedArray<int>& streamList)
{
	if (rse_first)
		rse_first->jrdStreamsCollector(streamList);

	if (rse_skip)
		rse_skip->jrdStreamsCollector(streamList);

	if (rse_boolean)
		rse_boolean->jrdStreamsCollector(streamList);

	// ASF: The legacy code used to visit rse_sorted and rse_projection, but the nod_sort was never
	// handled.
	// rse_sorted->jrdStreamsCollector(streamList);
	// rse_projection->jrdStreamsCollector(streamList);
}


//--------------------


// Parse a MAP clause for a union or global aggregate expression.
static MapNode* parseMap(thread_db* tdbb, CompilerScratch* csb, USHORT stream)
{
	SET_TDBB(tdbb);

	if (csb->csb_blr_reader.getByte() != blr_map)
		PAR_syntax_error(csb, "blr_map");

	int count = csb->csb_blr_reader.getWord();
	MapNode* node = FB_NEW(csb->csb_pool) MapNode(csb->csb_pool);

	while (count-- > 0)
	{
		node->targetList.add(PAR_gen_field(tdbb, stream, csb->csb_blr_reader.getWord()));
		node->sourceList.add(PAR_parse_value(tdbb, csb));
	}

	return node;
}

// Compare two strings, which could be either space-terminated or null-terminated.
static SSHORT strcmpSpace(const char* p, const char* q)
{
	for (; *p && *p != ' ' && *q && *q != ' '; p++, q++)
	{
		if (*p != *q)
			break;
	}

	if ((!*p || *p == ' ') && (!*q || *q == ' '))
		return 0;

	return (*p > *q) ? 1 : -1;
}

// Process a single record source stream from an RseNode.
// Obviously, if the source is a view, there is more work to do.
static void processSource(thread_db* tdbb, CompilerScratch* csb, RseNode* rse,
	RecordSourceNode* source, BoolExprNode** boolean, RecordSourceNodeStack& stack)
{
	SET_TDBB(tdbb);

	Database* dbb = tdbb->getDatabase();
	CHECK_DBB(dbb);

	AutoSetRestore<bool> autoValidateExpr(&csb->csb_validate_expr, false);

	source->pass1Source(tdbb, csb, rse, boolean, stack);
}

// Translate a map block into a format. If the format is missing or incomplete, extend it.
static void processMap(thread_db* tdbb, CompilerScratch* csb, MapNode* map, Format** inputFormat)
{
	SET_TDBB(tdbb);

	Format* format = *inputFormat;
	if (!format)
		format = *inputFormat = Format::newFormat(*tdbb->getDefaultPool(), map->sourceList.getCount());

	// process alternating rse and map blocks
	dsc desc2;
	NestConst<ValueExprNode>* source = map->sourceList.begin();
	NestConst<ValueExprNode>* target = map->targetList.begin();

	for (const NestConst<ValueExprNode>* const sourceEnd = map->sourceList.end();
		 source != sourceEnd;
		 ++source, ++target)
	{
		FieldNode* field = (*target)->as<FieldNode>();
		const USHORT id = field->fieldId;

		if (id >= format->fmt_count)
			format->fmt_desc.resize(id + 1);

		dsc* desc = &format->fmt_desc[id];
		(*source)->getDesc(tdbb, csb, &desc2);
		const USHORT min = MIN(desc->dsc_dtype, desc2.dsc_dtype);
		const USHORT max = MAX(desc->dsc_dtype, desc2.dsc_dtype);

		if (!min)
		{
			// eg: dtype_unknown
			*desc = desc2;
		}
		else if (max == dtype_blob)
		{
			desc->dsc_dtype = dtype_blob;
			desc->dsc_length = sizeof(ISC_QUAD);
			desc->dsc_scale = 0;
			desc->dsc_sub_type = DataTypeUtil::getResultBlobSubType(desc, &desc2);
			desc->dsc_flags = 0;
		}
		else if (min <= dtype_any_text)
		{
			// either field a text field?
			const USHORT len1 = DSC_string_length(desc);
			const USHORT len2 = DSC_string_length(&desc2);
			desc->dsc_dtype = dtype_varying;
			desc->dsc_length = MAX(len1, len2) + sizeof(USHORT);

			// pick the max text type, so any transparent casts from ints are
			// not left in ASCII format, but converted to the richer text format

			desc->setTextType(MAX(INTL_TEXT_TYPE(*desc), INTL_TEXT_TYPE(desc2)));
			desc->dsc_scale = 0;
			desc->dsc_flags = 0;
		}
		else if (DTYPE_IS_DATE(max) && !DTYPE_IS_DATE(min))
		{
			desc->dsc_dtype = dtype_varying;
			desc->dsc_length = DSC_convert_to_text_length(max) + sizeof(USHORT);
			desc->dsc_ttype() = ttype_ascii;
			desc->dsc_scale = 0;
			desc->dsc_flags = 0;
		}
		else if (max != min)
		{
			// different numeric types: if one is inexact use double,
			// if both are exact use int64
			if ((!DTYPE_IS_EXACT(max)) || (!DTYPE_IS_EXACT(min)))
			{
				desc->dsc_dtype = DEFAULT_DOUBLE;
				desc->dsc_length = sizeof(double);
				desc->dsc_scale = 0;
				desc->dsc_sub_type = 0;
				desc->dsc_flags = 0;
			}
			else
			{
				desc->dsc_dtype = dtype_int64;
				desc->dsc_length = sizeof(SINT64);
				desc->dsc_scale = MIN(desc->dsc_scale, desc2.dsc_scale);
				desc->dsc_sub_type = MAX(desc->dsc_sub_type, desc2.dsc_sub_type);
				desc->dsc_flags = 0;
			}
		}
	}

	// flesh out the format of the record

	ULONG offset = FLAG_BYTES(format->fmt_count);

	Format::fmt_desc_iterator desc3 = format->fmt_desc.begin();
	for (const Format::fmt_desc_const_iterator end_desc = format->fmt_desc.end();
		 desc3 < end_desc; ++desc3)
	{
		const USHORT align = type_alignments[desc3->dsc_dtype];

		if (align)
			offset = FB_ALIGN(offset, align);

		desc3->dsc_address = (UCHAR*)(IPTR) offset;
		offset += desc3->dsc_length;
	}

	if (offset > MAX_MESSAGE_SIZE)
		ERR_post(Arg::Gds(isc_imp_exc) << Arg::Gds(isc_blktoobig));

	format->fmt_length = offset;
	format->fmt_count = format->fmt_desc.getCount();
}

// Make new boolean nodes from nodes that contain a field from the given shellStream.
// Those fields are references (mappings) to other nodes and are used by aggregates and union rse's.
static void genDeliverUnmapped(thread_db* tdbb, BoolExprNodeStack* deliverStack, MapNode* map,
	BoolExprNodeStack* parentStack, UCHAR shellStream)
{
	SET_TDBB(tdbb);

	for (BoolExprNodeStack::iterator stack1(*parentStack); stack1.hasData(); ++stack1)
	{
		BoolExprNode* boolean = stack1.object();

		// Reduce to simple comparisons

		ComparativeBoolNode* cmpNode = boolean->as<ComparativeBoolNode>();
		MissingBoolNode* missingNode = boolean->as<MissingBoolNode>();
		HalfStaticArray<ValueExprNode*, 2> children;

		if (cmpNode &&
			(cmpNode->blrOp == blr_eql || cmpNode->blrOp == blr_gtr || cmpNode->blrOp == blr_geq ||
			 cmpNode->blrOp == blr_leq || cmpNode->blrOp == blr_lss || cmpNode->blrOp == blr_starting))
		{
			children.add(cmpNode->arg1);
			children.add(cmpNode->arg2);
		}
		else if (missingNode)
			children.add(missingNode->arg);
		else
			continue;

		// At least 1 mapping should be used in the arguments
		size_t indexArg;
		bool mappingFound = false;

		for (indexArg = 0; (indexArg < children.getCount()) && !mappingFound; ++indexArg)
		{
			FieldNode* fieldNode = children[indexArg]->as<FieldNode>();

			if (fieldNode && fieldNode->fieldStream == shellStream)
				mappingFound = true;
		}

		if (!mappingFound)
			continue;

		// Create new node and assign the correct existing arguments

		BoolExprNode* deliverNode = NULL;
		HalfStaticArray<ValueExprNode**, 2> newChildren;

		if (cmpNode)
		{
			ComparativeBoolNode* newCmpNode = FB_NEW(*tdbb->getDefaultPool()) ComparativeBoolNode(
				*tdbb->getDefaultPool(), cmpNode->blrOp);

			newChildren.add(newCmpNode->arg1.getAddress());
			newChildren.add(newCmpNode->arg2.getAddress());

			deliverNode = newCmpNode;
		}
		else if (missingNode)
		{
			MissingBoolNode* newMissingNode = FB_NEW(*tdbb->getDefaultPool()) MissingBoolNode(
				*tdbb->getDefaultPool());

			newChildren.add(newMissingNode->arg.getAddress());

			deliverNode = newMissingNode;
		}

		deliverNode->nodFlags = boolean->nodFlags;
		deliverNode->impureOffset = boolean->impureOffset;

		bool okNode = true;

		for (indexArg = 0; (indexArg < children.getCount()) && okNode; ++indexArg)
		{
			// Check if node is a mapping and if so unmap it, but only for root nodes (not contained
			// in another node). This can be expanded by checking complete expression (Then don't
			// forget to leave aggregate-functions alone in case of aggregate rse).
			// Because this is only to help using an index we keep it simple.

			FieldNode* fieldNode = children[indexArg]->as<FieldNode>();

			if (fieldNode && fieldNode->fieldStream == shellStream)
			{
				const USHORT fieldId = fieldNode->fieldId;

				if (fieldId >= map->sourceList.getCount())
					okNode = false;
				else
				{
					// Check also the expression inside the map, because aggregate
					// functions aren't allowed to be delivered to the WHERE clause.
					ValueExprNode* value = map->sourceList[fieldId];
					okNode = value->jrdUnmappableNode(map, shellStream);

					if (okNode)
						*newChildren[indexArg] = map->sourceList[fieldId];
				}
			}
			else
			{
				if ((okNode = children[indexArg]->jrdUnmappableNode(map, shellStream)))
					*newChildren[indexArg] = children[indexArg];
			}
		}

		if (!okNode)
			delete deliverNode;
		else
			deliverStack->push(deliverNode);
	}
}

// Mark indices that were not included in the user-specified access plan.
static void markIndices(CompilerScratch::csb_repeat* csbTail, SSHORT relationId)
{
	const PlanNode* plan = csbTail->csb_plan;

	if (!plan || plan->type != PlanNode::TYPE_RETRIEVE)
		return;

	// Go through each of the indices and mark it unusable
	// for indexed retrieval unless it was specifically mentioned
	// in the plan; also mark indices for navigational access.

	// If there were none indices, this is a sequential retrieval.

	index_desc* idx = csbTail->csb_idx->items;

	for (USHORT i = 0; i < csbTail->csb_indices; i++)
	{
		if (plan->accessType)
		{
			ObjectsArray<PlanNode::AccessItem>::iterator arg = plan->accessType->items.begin();
			const ObjectsArray<PlanNode::AccessItem>::iterator end = plan->accessType->items.end();

			for (; arg != end; ++arg)
			{
				if (relationId != arg->relationId)
				{
					// index %s cannot be used in the specified plan
					ERR_post(Arg::Gds(isc_index_unused) << arg->indexName);
				}

				if (idx->idx_id == arg->indexId)
				{
					if (plan->accessType->type == PlanNode::AccessType::TYPE_NAVIGATIONAL &&
						arg == plan->accessType->items.begin())
					{
						// dimitr:	navigational access can use only one index,
						//			hence the extra check added (see the line above)
						idx->idx_runtime_flags |= idx_plan_navigate;
					}
					else
					{
						// nod_indices
						break;
					}
				}
			}

			if (arg == end)
				idx->idx_runtime_flags |= idx_plan_dont_use;
		}
		else
			idx->idx_runtime_flags |= idx_plan_dont_use;

		++idx;
	}
}

// Resolve a field for JOIN USING purposes.
static dsql_nod* resolveUsingField(DsqlCompilerScratch* dsqlScratch, dsql_str* name,
	DsqlNodStack& stack, const dsql_nod* flawedNode, const TEXT* side, dsql_ctx*& ctx)
{
	dsql_nod* list = MAKE_list(stack);
	dsql_nod* node = PASS1_lookup_alias(dsqlScratch, name, list, false);

	if (!node)
	{
		string qualifier;
		qualifier.printf("<%s side of USING>", side);
		PASS1_field_unknown(qualifier.c_str(), name->str_data, flawedNode);
	}

	FieldNode* fieldNode;
	DerivedFieldNode* derivedField;

	if ((fieldNode = ExprNode::as<FieldNode>(node)))
	{
		ctx = fieldNode->dsqlContext;
		return node;
	}
	else if ((derivedField = ExprNode::as<DerivedFieldNode>(node)))
	{
		ctx = derivedField->context;
		return node;
	}

	if (node->nod_type == Dsql::nod_alias)
	{
		fb_assert(node->nod_count >= (int) Dsql::e_alias_imp_join - 1);
		ctx = reinterpret_cast<ImplicitJoin*>(node->nod_arg[Dsql::e_alias_imp_join])->visibleInContext;
	}
	else
	{
		fb_assert(false);
	}

	return node;
}

// Sort SortedStream indices based on there selectivity. Lowest selectivy as first, highest as last.
static void sortIndicesBySelectivity(CompilerScratch::csb_repeat* csbTail)
{
	if (csbTail->csb_plan)
		return;

	index_desc* selectedIdx = NULL;
	Array<index_desc> idxSort(csbTail->csb_indices);
	bool sameSelectivity = false;

	// Walk through the indices and sort them into into idxSort
	// where idxSort[0] contains the lowest selectivity (best) and
	// idxSort[csbTail->csb_indices - 1] the highest (worst)

	if (csbTail->csb_idx && (csbTail->csb_indices > 1))
	{
		for (USHORT j = 0; j < csbTail->csb_indices; j++)
		{
			float selectivity = 1; // Maximum selectivity is 1 (when all keys are the same)
			index_desc* idx = csbTail->csb_idx->items;

			for (USHORT i = 0; i < csbTail->csb_indices; i++)
			{
				// Prefer ASC indices in the case of almost the same selectivities
				if (selectivity > idx->idx_selectivity)
					sameSelectivity = ((selectivity - idx->idx_selectivity) <= 0.00001);
				else
					sameSelectivity = ((idx->idx_selectivity - selectivity) <= 0.00001);

				if (!(idx->idx_runtime_flags & idx_marker) &&
					 (idx->idx_selectivity <= selectivity) &&
					 !((idx->idx_flags & idx_descending) && sameSelectivity))
				{
					selectivity = idx->idx_selectivity;
					selectedIdx = idx;
				}

				++idx;
			}

			// If no index was found than pick the first one available out of the list
			if ((!selectedIdx) || (selectedIdx->idx_runtime_flags & idx_marker))
			{
				idx = csbTail->csb_idx->items;

				for (USHORT i = 0; i < csbTail->csb_indices; i++)
				{
					if (!(idx->idx_runtime_flags & idx_marker))
					{
						selectedIdx = idx;
						break;
					}

					++idx;
				}
			}

			selectedIdx->idx_runtime_flags |= idx_marker;
			idxSort.add(*selectedIdx);
		}

		// Finally store the right order in cbs_tail->csb_idx
		index_desc* idx = csbTail->csb_idx->items;

		for (USHORT j = 0; j < csbTail->csb_indices; j++)
		{
			idx->idx_runtime_flags &= ~idx_marker;
			memcpy(idx, &idxSort[j], sizeof(index_desc));
			++idx;
		}
	}
}