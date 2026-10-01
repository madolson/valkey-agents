use crate::error::{Error, StdError, span_result};
use crate::config::{Config, GlobalOpts, KvOpts};
use serde::{de, serde_if_integer128, forward_to_deserialize_any};
use de::{Visitor};
use proc_macro2::Span;
use syn::token;
use syn::parse::{Parse, ParseStream, ParseBuffer};
use syn::spanned::Spanned;
use syn::punctuated::Punctuated;
use std::str::FromStr;
use bitflags::bitflags;

/// Implemented by integers that we want to deserialze from `LitInt`.
trait IntType: Sized + FromStr where <Self as FromStr>::Err: StdError {
    const SUFFIX: &'static str;

    fn neg(self, span: Span) -> Result<Self, syn::Error>;
}

macro_rules! impl_int_type {
    (u, $suffix:ident; $($rest:tt)*) => {
        impl IntType for $suffix {
            const SUFFIX: &'static str = stringify!($suffix);

            fn neg(self, span: Span) -> Result<Self, syn::Error> {
                Err(syn::Error::new(span, "integer is unsigned"))
            }
        }
        impl_int_type!($($rest)*);
    };
    (i, $suffix:ident; $($rest:tt)*) => {
        impl IntType for $suffix {
            const SUFFIX: &'static str = stringify!($suffix);

            fn neg(self, _: Span) -> Result<Self, syn::Error> {
                Ok(-self)
            }
        }
        impl_int_type!($($rest)*);
    };
    () => { };
}

impl_int_type!(
    i, i8;
    u, u8;
    i, i16;
    u, u16;
    i, i32;
    u, u32;
    i, i64;
    u, u64;
);

serde_if_integer128! { impl_int_type!(
    i, i128;
    u, u128;
); }

/// Implemented by floats that we want to deserialze from `LitFloat`.
trait FloatType: Sized + FromStr where <Self as FromStr>::Err: StdError {
    const SUFFIX: &'static str;

    fn neg(self) -> Self;
}

impl FloatType for f32 {
    const SUFFIX: &'static str = "f32";

    fn neg(self) -> Self { -self }
}

impl FloatType for f64 {
    const SUFFIX: &'static str = "f64";

    fn neg(self) -> Self { -self }
}

/// A wrapper for integers that implements `Parse`.
struct IPrim<T>(T, Span);
impl<T: IntType> Parse for IPrim<T> where <T as FromStr>::Err: StdError {
    fn parse(stream: ParseStream) -> syn::Result<Self> {
        let neg = match stream.peek(token::Sub) {
            true => Some(stream.parse::<token::Sub>()?.span),
            false => None,
        };
        let lit: syn::LitInt = stream.parse()?;
        if !lit.suffix().is_empty() && lit.suffix() != T::SUFFIX {
            return Err(syn::Error::new(lit.span(),
                format!("expected {}", T::SUFFIX)));
        }
        let mut val = lit.base10_parse::<T>()?;
        if let Some(span) = neg {
            val = val.neg(span)?;
        }
        Ok(IPrim(val, lit.span()))
    }
}

/// A wrapper for floats that implements `Parse`.
struct FPrim<T>(T, Span);
impl<T: FloatType> Parse for FPrim<T> where <T as FromStr>::Err: StdError {
    fn parse(stream: ParseStream) -> syn::Result<Self> {
        let is_neg = stream.peek(token::Sub);
        if is_neg {
            stream.parse::<token::Sub>()?;
        }
        let lit: syn::LitFloat = stream.parse()?;
        if !lit.suffix().is_empty() && lit.suffix() != T::SUFFIX {
            return Err(syn::Error::new(lit.span(),
                format!("expected {}", T::SUFFIX)));
        }
        let mut val = lit.base10_parse::<T>()?;
        if is_neg {
            val = val.neg();
        }
        Ok(FPrim(val, lit.span()))
    }
}

bitflags!(
    struct InPosition: u8 {
        /// Tokens are already grouped. This is usually because of some
        /// attribute-like syntax: key(grouped_value).
        const GROUPED = 0b0001;

        /// Tokens are at the root of the input.
        const ROOT = 0b0010;

        /// Tokens are expected to be an identifier value.
        const IDENT = 0b0100;

        /// Allow strings in this identifier position.
        const STRING_IDENT = 0b1000;
    }
);

/// An implementor of this trait can borrow or own a `syn::ParseBuffer`.
///
/// `'de` is the lifetime of the parse buffer itself while `'r` is some lifetime
/// in which the buffer can be referenced.
pub trait Tokens<'de: 'r, 'r>: 'r {
    fn as_ref(&self) -> &ParseBuffer<'de>;

    #[inline(always)]
    fn parse<T: Parse>(&self) -> syn::Result<T> {
        self.as_ref().parse()
    }
}

impl<'de: 'r, 'r> Tokens<'de, 'r> for ParseBuffer<'de> {
    #[inline(always)]
    fn as_ref(&self) -> &ParseBuffer<'de> { self }
}

impl<'de, 'r> Tokens<'de, 'r> for &'r ParseBuffer<'de> {
    #[inline(always)]
    fn as_ref(&self) -> &ParseBuffer<'de> { *self }
}

impl<'de: 'r, 'r, T: Tokens<'de, 'r>> Tokens<'de, 'r> for Deserializer<T> {
    #[inline(always)]
    fn as_ref(&self) -> &ParseBuffer<'de> { self.tokens.as_ref() }
}

struct SeqAccess<T> {
    tokens: Deserializer<T>,
}

impl<'de: 'r, 'r, T: Tokens<'de, 'r>> de::SeqAccess<'de> for SeqAccess<T> {
    type Error = Error;

    fn next_element_seed<S: de::DeserializeSeed<'de>>(&mut self, seed: S)
        -> Result<Option<S::Value>, Error>
    {
        if self.tokens.as_ref().is_empty() { return Ok(None) }
        let out = seed.deserialize(self.tokens.copy())?;
        if !self.tokens.as_ref().is_empty() { self.tokens.parse::<token::Comma>()?; }
        Ok(Some(out))
    }
}

struct MapAccess<'de, T> {
    d: Deserializer<T>,
    opts: KvOpts,
    next_value: Option<BufferDeserializer<'de>>,
    is_unit: bool,
}

impl<'de: 'r, 'r, T: Tokens<'de, 'r>> MapAccess<'de, T> {
    fn key_deser<'b>(&self, buf: &'b Deserializer<impl Tokens<'de, 'r>>, ident: bool)
        -> BufferRefDeserializer<'de, 'b>
    {
        let mut d = buf.copy();
        if ident {
            // The key will be an identifier. Configure the deserializer
            // accordingly.
            d.flags.set(
                InPosition::STRING_IDENT,
                self.opts.contains(KvOpts::STRING_IDENTS));
            d.flags.set(InPosition::IDENT, true);
        }
        d
     }
}

impl<'de: 'r, 'r, T: Tokens<'de, 'r>> de::MapAccess<'de> for MapAccess<'de, T> {
    type Error = Error;

    fn next_key_seed<S: de::DeserializeSeed<'de>>(&mut self, seed: S)
        -> Result<Option<S::Value>, Error>
    {

        // Check done
        if self.d.as_ref().is_empty() { return Ok(None) }

        // Setup useful stuff
        let opts = self.opts;
        let cfged = |o: KvOpts| opts.intersects(o);

        // Check for tuple array
        if cfged(KvOpts::TUPLE_ARRAY) && self.d.as_ref().peek(token::Paren) {
            let buf = self.d.in_parens()?;
            self.next_value = Some(buf);
            let d = self.next_value.as_ref().unwrap();
            let out = seed.deserialize(self.key_deser(d, false))?;
            d.parse::<token::Comma>()?;
            return Ok(Some(out));
        }

        // Parse key
        let key_is_ident = self.d.as_ref().peek(syn::LitStr)
            || self.d.as_ref().peek(syn::Ident)
            && (self.d.as_ref().peek2(token::Colon)
                || self.d.as_ref().peek2(token::Eq)
                || self.d.as_ref().peek2(token::Paren)
            );
        let out = seed.deserialize(self.key_deser(&self.d, key_is_ident))?;

        // Parse delineator
        let lh = self.d.as_ref().lookahead1();
        if key_is_ident &&
            cfged(KvOpts::STRUCT_LIKE) &&
            lh.peek(token::Colon)
        {
            self.d.parse::<token::Colon>()?;
        } else if cfged(KvOpts::MATCH_LIKE) &&
            lh.peek(token::FatArrow)
        {
            self.d.parse::<token::FatArrow>()?;
        } else if key_is_ident &&
            cfged(KvOpts::STATEMENT_LIKE) &&
            lh.peek(token::Eq)
        {
            self.d.parse::<token::Eq>()?;
        } else if key_is_ident &&
            cfged(KvOpts::ATTRIBUTE_LIKE) &&
            lh.peek(token::Paren)
        {
            let mut buf = self.d.in_parens()?;
            buf.flags.insert(InPosition::GROUPED);
            self.next_value = Some(buf);
        } else if cfged(KvOpts::BARE_UNIT) &&
            (lh.peek(token::Comma) || self.d.as_ref().cursor().eof())
        {
            self.is_unit = true;
        } else {
            return Err(lh.error().into());
        }

        // Done
        Ok(Some(out))
    }

    fn next_value_seed<S: de::DeserializeSeed<'de>>(&mut self, seed: S)
        -> Result<S::Value, Error>
    {
        let out;
        if self.is_unit {
            self.is_unit = false;
            out = seed.deserialize(UnitDeserializer)?;
        } else {
            out = match self.next_value.take() {
                Some(d) => seed.deserialize(d)?,
                None => seed.deserialize(self.d.copy())?,
            };
        }
        if !self.d.as_ref().is_empty() { self.d.parse::<token::Comma>()?; }
        Ok(out)
    }
}

struct EnumAccess<T> {
    d: Deserializer<T>,
}

impl<'de: 'r, 'r, T: Tokens<'de, 'r>> de::EnumAccess<'de> for EnumAccess<T> {
    type Error = Error;
    type Variant = Self;

    fn variant_seed<V>(self, seed: V)
        -> Result<(V::Value, Self::Variant), Self::Error>
    where
        V: de::DeserializeSeed<'de>
    {
        Ok((seed.deserialize(self.d.copy())?, self))
    }
}

impl<'de: 'r, 'r, T: Tokens<'de, 'r>> de::VariantAccess<'de> for EnumAccess<T> {
    type Error = Error;

    fn unit_variant(self) -> Result<(), Self::Error> {
        Ok(())
    }

    fn newtype_variant_seed<S>(self, seed: S) -> Result<S::Value, Self::Error>
    where
        S: de::DeserializeSeed<'de>
    {
        let d = self.d.in_parens()?;
        let span = d.span;
        span_result(seed.deserialize(d), span)
    }

    fn tuple_variant<V>(self, len: usize, visitor: V) -> Result<V::Value, Self::Error>
    where
        V: Visitor<'de>
    {
        de::Deserializer::deserialize_tuple(self.d, len, visitor)
    }

    fn struct_variant<V>(
        self,
        _fields: &'static [&'static str],
        visitor: V,
    ) -> Result<V::Value, Self::Error>
    where
        V: Visitor<'de>
    {
        let opts = self.d.config.structs;
        self.d.maplike(opts, visitor)
    }
}

/// A generic structure that deserializes tokens from Syn into Rust values.
pub struct Deserializer<T> {
    config: Config,
    flags: InPosition,
    span: Span,
    tokens: T,
}

/// A `Deserializer` from Syn token streams into Rust values.
pub type StreamDeserializer<'de> = Deserializer<ParseStream<'de>>;

/// A `Deserializer` from Syn token buffers into Rust values.
pub type BufferDeserializer<'de> = Deserializer<ParseBuffer<'de>>;

type BufferRefDeserializer<'de, 'r> = Deserializer<&'r ParseBuffer<'de>>;

impl<'de: 'r, 'r, T: Tokens<'de, 'r>> Deserializer<T> {
    /// Create a syn deserializer which takes in a tokens from syn.
    ///
    /// Typically you should use one of:
    /// - `parser`
    /// - `from_stream`
    pub fn new(config: Config, tokens: T) -> Self {
        let mut d = Self::from(config, tokens.as_ref().cursor().span(), tokens);
        d.flags.set(InPosition::ROOT, true);
        d
    }

    /// Similar to `new` but assumes the deserializer isn't at the root of the
    /// input.
    fn from(config: Config, span: Span, tokens: T) -> Self {
        Deserializer {
            config,
            flags: InPosition::empty(),
            span,
            tokens,
        }
    }

    fn in_parens(&self) -> syn::Result<BufferDeserializer<'de>> {
        let content;
        let parens = syn::parenthesized!(content in *self.as_ref());
        Ok(Deserializer::from(self.config, parens.span, content))
    }

    fn in_brackets(&self) -> syn::Result<BufferDeserializer<'de>> {
        let content;
        let parens = syn::bracketed!(content in *self.as_ref());
        Ok(Deserializer::from(self.config, parens.span, content))
    }

    fn in_braces(&self) -> syn::Result<BufferDeserializer<'de>> {
        let content;
        let parens = syn::braced!(content in *self.as_ref());
        Ok(Deserializer::from(self.config, parens.span, content))
    }

    fn ident(&self, ident: &str) -> syn::Result<Span> {
        let token: syn::Ident = self.parse()?;
        if token != ident {
            return Err(syn::Error::new(
                token.span(),
                format!("expected {}", ident)));
        }
        Ok(token.span())
    }

    fn copy(&self) -> BufferRefDeserializer<'de, '_> {
        Deserializer::from(self.config, self.span, self.as_ref())
    }

    fn fork(&self) -> BufferDeserializer<'de> {
        Deserializer::from(self.config, self.span, self.as_ref().fork())
    }

    fn consume_struct_prefix(&mut self, name: &str) -> syn::Result<bool> {
        if self.gopt(GlobalOpts::BARE_STRUCTS) {
            if self.fork().ident(name).is_err() { return Ok(false); }
        }
        self.before_grouped();
        self.ident(name)?;
        Ok(true)
    }

    fn consume_enum_prefix(&self, name: &str) -> syn::Result<()> {
        if self.gopt(GlobalOpts::BARE_ENUMS) && self.fork().ident(name).is_err() {
            if self.as_ref().peek(token::Colon2) {
                self.parse::<token::Colon2>()?;
            }
        } else {
            self.ident(name)?;
            self.parse::<token::Colon2>()?;
        }
        Ok(())
    }

    fn gopt(&self, opt: GlobalOpts) -> bool {
        self.config.global.contains(opt)
    }

    fn is_grouped(&self) -> bool {
        self.gopt(GlobalOpts::UNGROUPED_ROOT) &&
        self.flags.contains(InPosition::ROOT) ||
        self.flags.contains(InPosition::GROUPED)
    }

    fn before_grouped(&mut self) {
        if self.gopt(GlobalOpts::UNGROUPED_ROOT) {
            self.flags.remove(InPosition::ROOT);
        }
        self.flags.remove(InPosition::GROUPED);
    }

    fn maplike<V>(self, mut opts: KvOpts, visitor: V) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        if self.is_grouped() {
            return visitor.visit_map(MapAccess {
                d: self.copy(),
                opts,
                next_value: None,
                is_unit: false,
            });
        }
        let braced =
            KvOpts::STRUCT_LIKE |
            KvOpts::STATEMENT_LIKE |
            KvOpts::MATCH_LIKE;
        let lh = self.as_ref().lookahead1();
        let content = if opts.intersects(braced | KvOpts::ATTRIBUTE_LIKE)
            && lh.peek(token::Brace)
        {
            opts -= KvOpts::TUPLE_ARRAY;
            self.in_braces()
        } else if opts.intersects(KvOpts::TUPLE_ARRAY | KvOpts::ATTRIBUTE_LIKE)
            && lh.peek(token::Bracket)
        {
            opts -= braced;
            self.in_brackets()
        } else if opts.intersects(KvOpts::ATTRIBUTE_LIKE) && lh.peek(token::Paren) {
            opts -= braced | KvOpts::TUPLE_ARRAY;
            self.in_parens()
        } else {
            Err(lh.error())
        }?;
        visitor.visit_map(MapAccess {
            d: content,
            opts,
            next_value: None,
            is_unit: false,
        })
    }
}

macro_rules! deserialize_lit {
    ($de:ident, $vi:ident, $ty:ty, $gen:ident) => {
        fn $de<V>(self, visitor: V) -> Result<V::Value, Error>
        where
            V: Visitor<'de>
        {
            let $gen(val, span) = $gen::<$ty>::parse(self.as_ref())?;
            span_result(visitor.$vi(val), span)
        }
    };
}

impl<'de: 'r, 'r, T: Tokens<'de, 'r>> de::Deserializer<'de> for Deserializer<T> {
    type Error = Error;

    fn deserialize_any<V>(self, _visitor: V) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        Err(syn::Error::new(
            self.as_ref().cursor().span(),
            "self-description is not yet supported").into())
    }

    fn deserialize_bool<V>(self, visitor: V) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        let lit: syn::LitBool = self.parse()?;
        span_result(visitor.visit_bool(lit.value), lit.span())
    }

    deserialize_lit!(deserialize_i8, visit_i8, i8, IPrim);
    deserialize_lit!(deserialize_u8, visit_u8, u8, IPrim);
    deserialize_lit!(deserialize_i16, visit_i16, i16, IPrim);
    deserialize_lit!(deserialize_u16, visit_u16, u16, IPrim);
    deserialize_lit!(deserialize_i32, visit_i32, i32, IPrim);
    deserialize_lit!(deserialize_u32, visit_u32, u32, IPrim);
    deserialize_lit!(deserialize_i64, visit_i64, i64, IPrim);
    deserialize_lit!(deserialize_u64, visit_u64, u64, IPrim);
    serde_if_integer128! {
        deserialize_lit!(deserialize_i128, visit_i128, i128, IPrim);
        deserialize_lit!(deserialize_u128, visit_u128, u128, IPrim);
    }
    deserialize_lit!(deserialize_f32, visit_f32, f32, FPrim);
    deserialize_lit!(deserialize_f64, visit_f64, f64, FPrim);

    fn deserialize_char<V>(self, visitor: V) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        let lit: syn::LitChar = self.parse()?;
        span_result(visitor.visit_char(lit.value()), lit.span())
    }

    fn deserialize_str<V>(self, visitor: V) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        self.deserialize_string(visitor)
    }

    fn deserialize_string<V>(self, visitor: V) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        if self.flags.contains(InPosition::IDENT) {
            self.deserialize_identifier(visitor)
        } else {
            let lit: syn::LitStr = self.parse()?;
            span_result(visitor.visit_string(lit.value()), lit.span())
        }
    }

    fn deserialize_bytes<V>(self, visitor: V) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        self.deserialize_byte_buf(visitor)
    }

    fn deserialize_byte_buf<V>(self, visitor: V) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        let lh = self.as_ref().lookahead1();
        if lh.peek(syn::LitByteStr) {
            let bytes: syn::LitByteStr = self.parse()?;
            span_result(visitor.visit_byte_buf(bytes.value()), bytes.span())
        } else if lh.peek(token::Bracket) {
            let d = self.in_brackets()?;
            let bytes: Punctuated<IPrim<u8>, token::Comma> =
                Punctuated::parse_terminated(&d.tokens)?;
            span_result(
                visitor.visit_byte_buf(bytes.iter().map(|x| x.0).collect()),
                d.span)
        } else {
            return Err(lh.error().into());
        }
    }

    fn deserialize_option<V>(self, visitor: V) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {

        let stream = self.as_ref();
        let forked = self.fork();
        let mut kw: syn::Ident;
        if stream.peek(token::Colon2) {
            stream.parse::<token::Colon2>()?;
            kw = stream.parse()?;
        } else if !stream.peek(syn::Ident) && self.gopt(GlobalOpts::BARE_OPTION) {
            return visitor.visit_some(self);
        } else {
            kw = stream.parse()?;
            if kw == "Option" {
                stream.parse::<token::Colon2>()?;
                kw = stream.parse()?;
            }
        }

        if kw == "Some" {
            let d = self.in_parens()?;
            let span = d.span;
            span_result(visitor.visit_some(d), span)
        } else if kw == "None" {
            span_result(visitor.visit_none(), kw.span())
        } else if self.gopt(GlobalOpts::BARE_OPTION) {
            return visitor.visit_some(forked);
        } else {
            return Err(syn::Error::new(kw.span(), "expected one of Option, Some, None").into());
        }
    }

    fn deserialize_unit<V>(self, visitor: V) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        let d = self.in_parens()?;
        span_result(visitor.visit_unit(), d.span)
    }

    fn deserialize_unit_struct<V>(
        self,
        name: &'static str,
        visitor: V,
    ) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        if self.gopt(GlobalOpts::BARE_STRUCTS) {
            let lh = self.as_ref().lookahead1();
            if lh.peek(token::Paren) {
                return self.deserialize_unit(visitor);
            } else if lh.peek(syn::Ident) { }
            else { return Err(lh.error().into()) }
        }
        let span = self.ident(name)?;
        span_result(visitor.visit_unit(), span)
    }

    fn deserialize_newtype_struct<V>(
        mut self,
        name: &'static str,
        visitor: V,
    ) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        if !self.consume_struct_prefix(name)? {
            return visitor.visit_newtype_struct(self);
        }
        let d = self.in_parens()?;
        let span = d.span;
        span_result(visitor.visit_newtype_struct(d), span)
    }

    fn deserialize_seq<V>(self, visitor: V) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        if self.is_grouped() {
            return visitor.visit_seq(SeqAccess { tokens: self.copy() });
        }
        let d = self.in_brackets()?;
        let span = d.span;
        span_result(visitor.visit_seq(SeqAccess {
            tokens: d,
        }), span)
    }

    fn deserialize_tuple<V>(self, _len: usize, visitor: V) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        if self.is_grouped() {
            return visitor.visit_seq(SeqAccess { tokens: self.copy() });
        }
        let d = self.in_parens()?;
        let span = d.span;
        span_result(visitor.visit_seq(SeqAccess {
            tokens: d,
        }), span)
    }

    fn deserialize_tuple_struct<V>(
        mut self,
        name: &'static str,
        len: usize,
        visitor: V,
    ) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        self.consume_struct_prefix(name)?;
        self.deserialize_tuple(len, visitor)
    }

    fn deserialize_map<V>(self, visitor: V) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        let opts = self.config.maps;
        self.maplike(opts, visitor)
    }

    fn deserialize_struct<V>(
        mut self,
        name: &'static str,
        _fields: &'static [&'static str],
        visitor: V,
    ) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        self.consume_struct_prefix(name)?;
        let opts = self.config.structs;
        self.maplike(opts, visitor)
    }

    fn deserialize_enum<V>(
        mut self,
        name: &'static str,
        _variants: &'static [&'static str],
        visitor: V,
    ) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        self.consume_enum_prefix(name)?;
        self.before_grouped();
        visitor.visit_enum(EnumAccess { d: self })
    }

    fn deserialize_identifier<V>(self, visitor: V) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        let lh = self.as_ref().lookahead1();
        let allow_str = match self.flags.contains(InPosition::IDENT) {
            true => self.flags.contains(InPosition::STRING_IDENT),
            false => self.gopt(GlobalOpts::STRING_IDENTS),
        };
        if allow_str && lh.peek(syn::LitStr) {
            let token: syn::LitStr = self.parse()?;
            span_result(visitor.visit_string(token.value()), token.span())
        } else if lh.peek(syn::Ident) {
            let token: syn::Ident = self.parse()?;
            span_result(visitor.visit_string(token.to_string()), token.span())
        } else {
            Err(lh.error().into())
        }
    }

    fn deserialize_ignored_any<V>(self, visitor: V) -> Result<V::Value, Error>
    where
        V: Visitor<'de>
    {
        self.deserialize_any(visitor)
    }
}

struct UnitDeserializer;

impl<'de> de::Deserializer<'de> for UnitDeserializer {
    type Error = Error;

    forward_to_deserialize_any! {
        bool i8 i16 i32 i64 i128 u8 u16 u32 u64 u128 f32 f64 char str string
        bytes byte_buf unit unit_struct newtype_struct seq tuple tuple_struct
        map struct enum identifier ignored_any
    }

    fn deserialize_any<V>(self, visitor: V) -> Result<V::Value, Self::Error>
    where
        V: de::Visitor<'de>,
    {
        visitor.visit_unit()
    }

    fn deserialize_option<V>(self, visitor: V) -> Result<V::Value, Self::Error>
    where
        V: de::Visitor<'de>,
    {
        visitor.visit_some(self)
    }
}
