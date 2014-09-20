/*
 * pg_type_map_by_oid.c - PG::TypeMapByOid class extension
 * $Id$
 *
 */

#include "pg.h"

static VALUE rb_cTypeMapByOid;
static ID s_id_decode;

typedef struct {
	t_typemap typemap;
	int max_rows_for_online_lookup;

	struct pg_tmbo_converter {
		VALUE oid_to_coder;

		struct pg_tmbo_oid_cache_entry {
			Oid oid;
			t_pg_coder *p_coder;
		} cache_row[0x100];
	} format[2];
} t_tmbo;

/*
 * We use the OID's minor 8 Bits as index to a 256 entry cache. This avoids full ruby hash lookups
 * for each value in most cases.
 */
#define CACHE_LOOKUP(this, form, oid) ( &this->format[(form)].cache_row[(oid) & 0xff] )

static t_pg_coder *
pg_tmbo_typecast_query_param(VALUE self, VALUE param_value, int field)
{
	rb_raise( rb_eNotImpError, "type map %s is not suitable to map query params", RSTRING_PTR(rb_inspect(self)) );
	return NULL;
}

static t_pg_coder *
pg_tmbo_lookup_oid(t_tmbo *this, int format, Oid oid)
{
	t_pg_coder *conv;
	struct pg_tmbo_oid_cache_entry *p_ce;

	p_ce = CACHE_LOOKUP(this, format, oid);

	/* Has the entry the expected OID and is it a non empty entry? */
	if( p_ce->oid == oid && (oid || p_ce->p_coder) ) {
		conv = p_ce->p_coder;
	} else {
		VALUE obj = rb_hash_lookup( this->format[format].oid_to_coder, UINT2NUM( oid ));
		/* obj must be nil or some kind of PG::Coder, this is checked at insertion */
		conv = NIL_P(obj) ? NULL : DATA_PTR(obj);
		/* Write the retrieved coder to the cache */
		p_ce->oid = oid;
		p_ce->p_coder = conv;
	}
	return conv;
}

static VALUE
pg_tmbo_result_value(VALUE self, PGresult *pgresult, int tuple, int field, t_typemap *p_typemap)
{
	VALUE ret;
	char * val;
	int len;
	int format;
	t_tmbo *this = (t_tmbo *) p_typemap;
	t_pg_coder *p_coder;

	if (PQgetisnull(pgresult, tuple, field)) {
		return Qnil;
	}

	val = PQgetvalue( pgresult, tuple, field );
	len = PQgetlength( pgresult, tuple, field );
	format = PQfformat( pgresult, field );

	if( format < 0 || format > 1 )
		rb_raise(rb_eArgError, "result field %d has unsupported format code %d", field+1, format);

	p_coder = pg_tmbo_lookup_oid( this, format, PQftype(pgresult, field) );

	if( p_coder && p_coder->dec_func ){
		return p_coder->dec_func(p_coder, val, len, tuple, field, ENCODING_GET(self));
	}

	if ( 0 == format ) {
		ret = pg_text_dec_string(NULL, val, len, tuple, field, ENCODING_GET(self));
	} else {
		ret = pg_bin_dec_bytea(NULL, val, len, tuple, field, ENCODING_GET(self));
	}

	if( p_coder ){
		ret = rb_funcall( p_coder->coder_obj, s_id_decode, 3, ret, INT2NUM(tuple), INT2NUM(field) );
	}

	return ret;
}

static VALUE
pg_tmbo_fit_to_query( VALUE params, VALUE self )
{
	rb_raise( rb_eNotImpError, "type map %s is not suitable to map query params", RSTRING_PTR(rb_inspect(self)) );
	return self;
}

static VALUE
pg_tmbo_fit_to_result( VALUE result, VALUE self )
{
	int nfields;
	int i;
	VALUE colmap;
	t_tmbo *this = DATA_PTR( self );
	PGresult *pgresult = pgresult_get( result );

	nfields = PQnfields( pgresult );

	if( PQntuples( pgresult ) <= this->max_rows_for_online_lookup ){
		/* do a hash lookup for each result value in pg_tmbc_result_value() */

		return self;
	}else{
		/* Build a TypeMapByColumn that fits to the given result */
		t_tmbc *p_colmap;

		p_colmap = xmalloc(sizeof(t_tmbc) + sizeof(struct pg_tmbc_converter) * nfields);
		colmap = pg_tmbc_allocate();
		DATA_PTR(colmap) = p_colmap;

		for(i=0; i<nfields; i++)
		{
			int format = PQfformat(pgresult, i);

			if( format < 0 || format > 1 )
				rb_raise(rb_eArgError, "result field %d has unsupported format code %d", i+1, format);

			p_colmap->convs[i].cconv = pg_tmbo_lookup_oid( this, format, PQftype(pgresult, i) );
		}

		/* encoding_index is set, when the TypeMapByColumn is assigned to a PG::Result. */
		p_colmap->nfields = nfields;
		p_colmap->typemap.fit_to_result = pg_tmbo_fit_to_result;
		p_colmap->typemap.fit_to_query = pg_tmbo_fit_to_query;
		p_colmap->typemap.typecast_result_value = pg_tmbc_result_value;
		p_colmap->typemap.typecast_query_param = pg_tmbo_typecast_query_param;

		return colmap;
	}
}

static void
pg_tmbo_mark( t_tmbo *this )
{
	int i;

	for( i=0; i<2; i++){
		rb_gc_mark(this->format[i].oid_to_coder);
	}
}

static VALUE
pg_tmbo_s_allocate( VALUE klass )
{
	t_tmbo *this;
	VALUE self;
	int i;

	self = Data_Make_Struct( klass, t_tmbo, pg_tmbo_mark, -1, this );

	this->typemap.fit_to_result = pg_tmbo_fit_to_result;
	this->typemap.fit_to_query = pg_tmbo_fit_to_query;
	this->typemap.typecast_result_value = pg_tmbo_result_value;
	this->typemap.typecast_query_param = pg_tmbo_typecast_query_param;
	this->max_rows_for_online_lookup = 10;

	for( i=0; i<2; i++){
		this->format[i].oid_to_coder = rb_hash_new();
	}

	return self;
}

static VALUE
pg_tmbo_add_coder( VALUE self, VALUE coder )
{
	VALUE hash;
	t_tmbo *this = DATA_PTR( self );
	t_pg_coder *p_coder;
	struct pg_tmbo_oid_cache_entry *p_ce;

	if( !rb_obj_is_kind_of(coder, rb_cPG_Coder) )
		rb_raise(rb_eArgError, "invalid type %s (should be some kind of PG::Coder)",
							rb_obj_classname( coder ));

	Data_Get_Struct(coder, t_pg_coder, p_coder);

	if( p_coder->format < 0 || p_coder->format > 1 )
		rb_raise(rb_eArgError, "invalid format code %d", p_coder->format);

	/* Update cache entry */
	p_ce = CACHE_LOOKUP(this, p_coder->format, p_coder->oid);
	p_ce->oid = p_coder->oid;
	p_ce->p_coder = p_coder;
	/* Write coder into the hash of the given format */
	hash = this->format[p_coder->format].oid_to_coder;
	rb_hash_aset( hash, UINT2NUM(p_coder->oid), coder);

	return self;
}

static VALUE
pg_tmbo_rm_coder( VALUE self, VALUE format, VALUE oid )
{
	VALUE hash;
	VALUE coder;
	t_tmbo *this = DATA_PTR( self );
	int i_format = NUM2INT(format);
	struct pg_tmbo_oid_cache_entry *p_ce;

	if( i_format < 0 || i_format > 1 )
		rb_raise(rb_eArgError, "invalid format code %d", i_format);

	/* Mark the cache entry as empty */
	p_ce = CACHE_LOOKUP(this, i_format, NUM2UINT(oid));
	p_ce->oid = 0;
	p_ce->p_coder = NULL;
	hash = this->format[i_format].oid_to_coder;
	coder = rb_hash_delete( hash, oid );

	return coder;
}

static VALUE
pg_tmbo_coders( VALUE self )
{
	t_tmbo *this = DATA_PTR( self );

	return rb_ary_concat(
			rb_funcall(this->format[0].oid_to_coder, rb_intern("values"), 0),
			rb_funcall(this->format[1].oid_to_coder, rb_intern("values"), 0));
}

static VALUE
pg_tmbo_max_rows_for_online_lookup_set( VALUE self, VALUE value )
{
	t_tmbo *this = DATA_PTR( self );
	this->max_rows_for_online_lookup = NUM2INT(value);
	return value;
}

static VALUE
pg_tmbo_max_rows_for_online_lookup_get( VALUE self )
{
	t_tmbo *this = DATA_PTR( self );
	return INT2NUM(this->max_rows_for_online_lookup);
}

void
init_pg_type_map_by_oid()
{
	s_id_decode = rb_intern("decode");

	rb_cTypeMapByOid = rb_define_class_under( rb_mPG, "TypeMapByOid", rb_cTypeMap );
	rb_define_alloc_func( rb_cTypeMapByOid, pg_tmbo_s_allocate );
	rb_define_method( rb_cTypeMapByOid, "add_coder", pg_tmbo_add_coder, 1 );
	rb_define_method( rb_cTypeMapByOid, "rm_coder", pg_tmbo_rm_coder, 2 );
	rb_define_method( rb_cTypeMapByOid, "coders", pg_tmbo_coders, 0 );
	rb_define_method( rb_cTypeMapByOid, "max_rows_for_online_lookup=", pg_tmbo_max_rows_for_online_lookup_set, 1 );
	rb_define_method( rb_cTypeMapByOid, "max_rows_for_online_lookup", pg_tmbo_max_rows_for_online_lookup_get, 0 );
}
