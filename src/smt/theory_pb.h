/*++
Copyright (c) 2013 Microsoft Corporation

Module Name:

    theory_pb.h

Abstract:

    Cardinality theory plugin.

Author:

    Nikolaj Bjorner (nbjorner) 2013-11-05

Notes:

    This custom theory handles cardinality constraints
    It performs unit propagation and switches to creating
    sorting circuits if it keeps having to propagate (create new clauses).
--*/

#include "smt_theory.h"
#include "pb_decl_plugin.h"
#include "smt_clause.h"
#include "theory_pb_params.h"
#include "simplex.h"

namespace smt {
    class theory_pb : public theory {

        struct psort_expr;
        class  pb_justification;
        class  pb_model_value_proc;
        class  unwatch_ge;
        class  rewatch_vars;
        class  negate_ineq;
        class  remove_var;
        class  undo_bound;

        class  card_justification;

        typedef rational numeral;
        typedef simplex::simplex<simplex::mpz_ext> simplex;
        typedef simplex::row row;
        typedef simplex::row_iterator row_iterator;
        typedef unsynch_mpq_inf_manager eps_manager;
        typedef _scoped_numeral<eps_manager> scoped_eps_numeral;

        struct arg_t : public vector<std::pair<literal, numeral> > {
            numeral         m_k;        // invariants: m_k > 0, coeffs[i] > 0

            unsigned get_hash() const;
            bool operator==(arg_t const& other) const;

            numeral const& k() const { return m_k; }

            struct hash {
                unsigned operator()(arg_t const& i) const { return i.get_hash(); }
            };
            struct eq {
                bool operator()(arg_t const& a, arg_t const& b) const { 
                    return a == b;
                }
            };
            struct child_hash {
                unsigned operator()(arg_t const& args, unsigned idx) const {
                    return args[idx].first.hash() ^ args[idx].second.hash();
                }
            };
            struct kind_hash {
                unsigned operator()(arg_t const& args) const {
                    return args.size();
                }
            };   

            void remove_negations();         

            void negate();

            lbool normalize(bool is_eq);

            void  prune(bool is_eq);
            
            literal lit(unsigned i) const { 
                return (*this)[i].first; 
            }

            numeral const & coeff(unsigned i) const { return (*this)[i].second; }

            std::ostream& display(context& ctx, std::ostream& out, bool values = false) const;

            app_ref to_expr(bool is_eq, context& ctx, ast_manager& m);

            bool well_formed() const;
        };

        struct stats {
            unsigned m_num_conflicts;
            unsigned m_num_propagations;
            unsigned m_num_predicates;
            unsigned m_num_compiles;
            unsigned m_num_compiled_vars;
            unsigned m_num_compiled_clauses;
            void reset() { memset(this, 0, sizeof(*this)); }
            stats() { reset(); }
        };


        struct ineq {
            unsynch_mpz_manager& m_mpz;    // mpz manager.
            literal         m_lit;      // literal repesenting predicate
            bool            m_is_eq;    // is this an = or >=.
            arg_t           m_args[2];  // encode args[0]*coeffs[0]+...+args[n-1]*coeffs[n-1] >= k();
            // Watch the first few positions until the sum satisfies:
            // sum coeffs[i] >= m_lower + max_watch            
            scoped_mpz      m_max_watch;    // maximal coefficient.
            unsigned        m_watch_sz;     // number of literals being watched.
            scoped_mpz      m_watch_sum;    // maximal sum of watch literals.
            // Watch infrastructure for = and unassigned >=:
            unsigned        m_nfixed;       // number of variables that are fixed.
            scoped_mpz      m_max_sum;      // maximal possible sum.
            scoped_mpz      m_min_sum;      // minimal possible sum.
            unsigned        m_num_propagations;
            unsigned        m_compilation_threshold;
            lbool           m_compiled;
            
            ineq(unsynch_mpz_manager& m, literal l, bool is_eq) : 
                m_mpz(m), m_lit(l), m_is_eq(is_eq), 
                m_max_watch(m), m_watch_sum(m), 
                m_max_sum(m), m_min_sum(m) {
                reset();
            }

            arg_t const& args() const { return m_args[m_lit.sign()]; }
            arg_t& args() { return m_args[m_lit.sign()]; }

            literal lit() const { return m_lit; }
            numeral const & k() const { return args().m_k; }
            mpz const & mpz_k() const { return k().to_mpq().numerator(); }

            literal lit(unsigned i) const { return args()[i].first; }
            numeral const & coeff(unsigned i) const { return args()[i].second; }
            class mpz const& ncoeff(unsigned i) const { return coeff(i).to_mpq().numerator(); }

            unsigned size() const { return args().size(); }

            scoped_mpz const& watch_sum() const { return m_watch_sum; }
            scoped_mpz const& max_watch() const { return m_max_watch; }
            void set_max_watch(mpz const& n) { m_max_watch = n; }
            unsigned watch_size() const { return m_watch_sz; }

            // variable watch infrastructure
            scoped_mpz const& min_sum() const { return m_min_sum; }
            scoped_mpz const& max_sum() const { return m_max_sum; }
            unsigned nfixed() const { return m_nfixed; }
            bool vwatch_initialized() const { return !m_mpz.is_zero(max_sum()); }
            void vwatch_reset() { m_min_sum.reset(); m_max_sum.reset(); m_nfixed = 0; }

            unsigned find_lit(bool_var v, unsigned begin, unsigned end) {
                while (lit(begin).var() != v) {
                    ++begin;
                    SASSERT(begin < end);
                }
                return begin;
            }

            void reset();

            void negate();

            lbool normalize();

            void unique();

            void prune();

            void post_prune();

            app_ref to_expr(context& ctx, ast_manager& m);

            bool is_eq() const { return m_is_eq; }
            bool is_ge() const { return !m_is_eq; }

        };

        // cardinality constraint args >= bound
        class card {
            literal         m_lit;      // literal repesenting predicate
            literal_vector  m_args;
            unsigned        m_bound;
            unsigned        m_num_propagations;
            unsigned        m_compilation_threshold;
            lbool           m_compiled;
            
        public:
            card(literal l, unsigned bound):
                m_lit(l),
                m_bound(bound),
                m_num_propagations(0),
                m_compilation_threshold(0),
                m_compiled(l_false)
            {
            }            
            
            literal lit() const { return m_lit; }
            literal lit(unsigned i) const { return m_args[i]; }
            unsigned k() const { return m_bound; }
            unsigned size() const { return m_args.size(); }
            unsigned num_propagations() const { return m_num_propagations; }
            void add_arg(literal l);
        
            void init_watch(theory_pb& th, bool is_true);

            lbool assign(theory_pb& th, literal lit);
        
            void negate();

            app_ref to_expr(context& ctx);

            void inc_propagations(theory_pb& th);
        private:

            bool validate_conflict(theory_pb& th);
            
            bool validate_assign(theory_pb& th, literal_vector const& lits, literal l);

            void set_conflict(theory_pb& th, literal l);
        };

        typedef ptr_vector<card> card_watch;
        typedef ptr_vector<ineq> ineq_watch;
        typedef map<arg_t, bool_var, arg_t::hash, arg_t::eq> arg_map;


        struct row_info {
            unsigned     m_slack;   // slack variable in simplex tableau
            numeral      m_bound;   // bound
            arg_t        m_rep;     // representative
            row_info(theory_var slack, numeral const& b, arg_t const& r):
                m_slack(slack), m_bound(b), m_rep(r) {}
            row_info(): m_slack(0) {}
        };


        struct var_info {
            ineq_watch*  m_lit_watch[2];
            ineq_watch*  m_var_watch;
            ineq*        m_ineq;

            card_watch*  m_lit_cwatch[2];
            card*        m_card;
            
            var_info(): m_var_watch(0), m_ineq(0), m_card(0)
            {
                m_lit_watch[0] = 0;
                m_lit_watch[1] = 0;
                m_lit_cwatch[0] = 0;
                m_lit_cwatch[1] = 0;
            }

            void reset() {
                dealloc(m_lit_watch[0]);
                dealloc(m_lit_watch[1]);
                dealloc(m_var_watch);
                dealloc(m_ineq);
                dealloc(m_lit_cwatch[0]);
                dealloc(m_lit_cwatch[1]);
                dealloc(m_card);
            }
        };


        theory_pb_params         m_params;        

        svector<var_info>        m_var_infos; 
        arg_map                  m_ineq_rep;       // Simplex: representative inequality
        u_map<row_info>          m_ineq_row_info;  // Simplex: row information per variable
        uint_set                 m_vars;           // Simplex: 0-1 variables.
        simplex                  m_simplex;        // Simplex: tableau
        literal_vector           m_explain_lower;  // Simplex: explanations for lower bounds
        literal_vector           m_explain_upper;  // Simplex: explanations for upper bounds
        unsynch_mpq_inf_manager  m_mpq_inf_mgr;    // Simplex: manage inf_mpq numerals
        mutable unsynch_mpz_manager      m_mpz_mgr;        // Simplex: manager mpz numerals
        unsigned_vector          m_ineqs_trail;
        unsigned_vector          m_ineqs_lim;
        literal_vector           m_literals;    // temporary vector
        pb_util                  m_util;
        stats                    m_stats;
        ptr_vector<ineq>         m_to_compile;  // inequalities to compile.
        unsigned                 m_conflict_frequency;
        bool                     m_learn_complements;
        bool                     m_enable_compilation;
        rational                 m_max_compiled_coeff;

        // internalize_atom:
        literal compile_arg(expr* arg);
        void init_watch(bool_var v);
        
        // general purpose pb constraints
        void add_watch(ineq& c, unsigned index);
        void del_watch(ineq_watch& watch, unsigned index, ineq& c, unsigned ineq_index);
        void init_watch_literal(ineq& c);
        void init_watch_var(ineq& c);
        void clear_watch(ineq& c);
        void watch_literal(literal lit, ineq* c);
        void watch_var(bool_var v, ineq* c);
        void unwatch_literal(literal w, ineq* c);
        void unwatch_var(bool_var v, ineq* c);
        void remove(ptr_vector<ineq>& ineqs, ineq* c);

        bool assign_watch_ge(bool_var v, bool is_true, ineq_watch& watch, unsigned index);
        void assign_watch(bool_var v, bool is_true, ineq& c);
        void assign_ineq(ineq& c, bool is_true);
        void assign_eq(ineq& c, bool is_true);

        // cardinality constraints
        // these are cheaper to handle than general purpose PB constraints
        // and in the common case PB constraints with small coefficients can
        // be handled using cardinality constraints.

        unsigned_vector          m_card_trail;
        unsigned_vector          m_card_lim;
        bool is_cardinality_constraint(app * atom);
        bool internalize_card(app * atom, bool gate_ctx);
        void card2conjunction(card const& c);

        void watch_literal(literal lit, card* c);
        void unwatch_literal(literal w, card* c);
        void add_clause(card& c, literal_vector const& lits);
        void add_assign(card& c, literal_vector const& lits, literal l);
        void remove(ptr_vector<card>& cards, card* c);
        void clear_watch(card& c);        
        std::ostream& display(std::ostream& out, card const& c, bool values = false) const;


        // simplex:
        bool check_feasible();

        std::ostream& display(std::ostream& out, ineq const& c, bool values = false) const;
        std::ostream& display(std::ostream& out, arg_t const& c, bool values = false) const;
        virtual void display(std::ostream& out) const;
        void display_watch(std::ostream& out, bool_var v, bool sign) const;
        void display_resolved_lemma(std::ostream& out) const;

        void add_clause(ineq& c, literal_vector const& lits);
        void add_assign(ineq& c, literal_vector const& lits, literal l);
        literal_vector& get_lits();

        literal_vector& get_all_literals(ineq& c, bool negate);
        literal_vector& get_helpful_literals(ineq& c, bool negate);
        literal_vector& get_unhelpful_literals(ineq& c, bool negate);

        //
        // Utilities to compile cardinality 
        // constraints into a sorting network.
        //
        void compile_ineq(ineq& c);
        void inc_propagations(ineq& c);
        unsigned get_compilation_threshold(ineq& c);

        //
        // Conflict resolution, cutting plane derivation.
        // 
        unsigned          m_num_marks;
        literal_vector    m_resolved;
        unsigned          m_conflict_lvl;

        // Conflict PB constraints
        svector<int>      m_coeffs;
        svector<bool_var> m_active_coeffs;
        int               m_bound;
        literal_vector    m_antecedents;
        uint_set          m_seen;
        unsigned_vector   m_seen_trail;

        void normalize_active_coeffs();
        void inc_coeff(literal l, int offset);
        int get_coeff(bool_var v) const;
        int get_abs_coeff(bool_var v) const;       
        int arg_max(uint_set& seen, int& coeff); 

        void reset_coeffs();
        literal cardinality_reduction(card*& c);

        bool resolve_conflict(card& c, literal_vector const& conflict_clause);
        void process_antecedent(literal l, int offset);
        void process_card(card& c, int offset);
        void cut();
        bool is_proof_justification(justification const& j) const;

        bool validate_lemma();

        void hoist_maximal_values();

        void validate_final_check();
        void validate_final_check(ineq& c);
        void validate_assign(ineq const& c, literal_vector const& lits, literal l) const;
        void validate_watch(ineq const& c) const;

        bool proofs_enabled() const { return get_manager().proofs_enabled(); }
        justification* justify(literal l1, literal l2);
        justification* justify(literal_vector const& lits);

    public:
        theory_pb(ast_manager& m, theory_pb_params& p);
        
        virtual ~theory_pb();

        virtual theory * mk_fresh(context * new_ctx);
        virtual bool internalize_atom(app * atom, bool gate_ctx);
        virtual bool internalize_term(app * term) { UNREACHABLE(); return false; }
        virtual void new_eq_eh(theory_var v1, theory_var v2);
        virtual void new_diseq_eh(theory_var v1, theory_var v2) { }
        virtual bool use_diseqs() const { return false; }
        virtual bool build_models() const { return false; }
        virtual final_check_status final_check_eh();
        virtual void reset_eh();
        virtual void assign_eh(bool_var v, bool is_true);
        virtual void init_search_eh();
        virtual void push_scope_eh();
        virtual void pop_scope_eh(unsigned num_scopes);
        virtual void restart_eh();
        virtual void collect_statistics(::statistics & st) const;
        virtual model_value_proc * mk_value(enode * n, model_generator & mg);
        virtual void init_model(model_generator & m);        
        virtual bool include_func_interp(func_decl* f) { return false; }

        static literal assert_ge(context& ctx, unsigned k, unsigned n, literal const* xs);
    };
};
