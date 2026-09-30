use crate::SynError;
use serde::de::Error as DeError;
use proc_macro2::Span;
use std::fmt;
pub(crate) use std::error::Error as StdError;

/// A serde deserialization error which wraps `syn::Error` or a custom string.
/// The `Syn` variant is preferred when possible, since it tracks the location
/// in the input syntax where the error occurred.
pub enum Error {
    Syn(SynError),
    Custom(String),
}

impl Error {
    /// Returns the location in the input syntax where this error occurred if
    /// one is known.
    pub fn span(&self) -> Option<Span> {
        match self {
            Error::Syn(e) => Some(e.span()),
            Error::Custom(_) => None,
        }
    }

    /// Attaches a span to this error if needed.
    #[inline(always)]
    pub fn with_span(self, span: Span) -> Error {
        Error::Syn(self.to_syn(span))
    }

    /// Converts `self` to a `syn::Error` by attaching a span if needed.
    ///
    /// ```
    /// # use proc_macro2::Span;
    /// # use serde_syn::Error;
    /// # use serde::de::Error as _;
    /// let err = Error::custom("an error");
    /// assert!(err.span().is_none());
    /// let syn_err = err.to_syn(Span::call_site());
    /// println!("{:?}", syn_err.span());
    /// ```
    #[inline(always)]
    pub fn to_syn(self, span: Span) -> SynError {
        match self {
            Error::Syn(e) => e,
            Error::Custom(msg) => SynError::new(span, msg),
        }
    }
}

/// Attach the given span to res if a span is not already attached.
#[inline(always)]
pub(crate) fn span_result<T>(res: Result<T, Error>, span: Span)
    -> Result<T, Error>
{
    res.map_err(|e| e.with_span(span))
}

impl From<SynError> for Error {
    fn from(err: SynError) -> Error {
        Error::Syn(err)
    }
}

impl DeError for Error {
    fn custom<T: fmt::Display>(msg: T) -> Self {
        Error::Custom(msg.to_string())
    }
}

impl StdError for Error {
    fn description(&self) -> &str {
        match self {
            Error::Syn(e) => e.description(),
            Error::Custom(m) => m.as_str(),
        }
    }

    fn source(&self) -> Option<&(dyn StdError + 'static)> {
        match self {
            Error::Syn(e) => Some(e),
            Error::Custom(_) => None,
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Error::Syn(e) => fmt::Display::fmt(e, f),
            Error::Custom(m) => fmt::Display::fmt(m, f),
        }
    }
}

impl fmt::Debug for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Error::Syn(e) => fmt::Debug::fmt(e, f),
            Error::Custom(m) => f.debug_tuple("Error")
                .field(&m)
                .finish(),
        }
    }
}
