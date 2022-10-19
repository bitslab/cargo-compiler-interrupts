//! Paths utilities

use std::path::{Path, PathBuf};

use anyhow::Context;

use crate::CIResult;

/// Extension trait for `AsRef<Path>`.
pub trait PathExt {
    /// Gets the file name of a path as a string.
    fn file_name(&self) -> CIResult<String>;

    /// Gets the file stem of a path as a string.
    fn file_stem(&self) -> CIResult<String>;

    /// Gets the file extension of a path as string.
    fn extension(&self) -> CIResult<String>;

    /// Gets the parent directory of a path.
    fn parent(&self) -> CIResult<PathBuf>;

    /// Converts a path to string.
    fn to_string(&self) -> CIResult<String>;

    /// Returns true if a path is executable.
    fn executable(&self) -> bool;

    /// Appends the suffix to the file stem of a path.
    fn append_suffix(&self, suffix: &str) -> CIResult<PathBuf>;

    /// Reads the directory for files matching the predicate.
    fn read_dir<P>(&self, predicate: P) -> CIResult<Vec<PathBuf>>
    where
        P: FnMut(&PathBuf) -> bool;
}

impl<T> PathExt for T
where
    T: AsRef<Path>,
{
    fn file_name(&self) -> CIResult<String> {
        let path = self.as_ref();
        path.file_name()
            .and_then(|s| s.to_str())
            .map(|s| s.to_string())
            .with_context(|| format!("failed to get file name `{}`", path.display()))
    }

    fn file_stem(&self) -> CIResult<String> {
        let path = self.as_ref();
        path.file_stem()
            .and_then(|s| s.to_str())
            .map(|s| s.to_string())
            .with_context(|| format!("failed to get file stem `{}`", path.display()))
    }

    fn extension(&self) -> CIResult<String> {
        let path = self.as_ref();
        path.extension()
            .and_then(|s| s.to_str())
            .map(|s| s.to_string())
            .with_context(|| format!("failed to get extension `{}`", path.display()))
    }

    fn parent(&self) -> CIResult<PathBuf> {
        let path = self.as_ref();
        Ok(path
            .parent()
            .with_context(|| format!("failed to get parent dir `{}`", path.display()))?
            .to_path_buf())
    }

    fn to_string(&self) -> CIResult<String> {
        let path = self.as_ref();
        path.to_str()
            .map(|s| s.to_string())
            .with_context(|| format!("failed to convert to string `{}`", path.display()))
    }

    fn executable(&self) -> bool {
        use std::os::unix::prelude::*;
        std::fs::metadata(self.as_ref())
            .map(|metadata| metadata.is_file() && metadata.permissions().mode() & 0o111 != 0)
            .unwrap_or(false)
    }

    fn append_suffix(&self, suffix: &str) -> CIResult<PathBuf> {
        let file_stem = self.file_stem()?;
        let extension = self.extension();
        let file_name = if let Ok(extension) = extension {
            format!("{}-{}.{}", file_stem, suffix, extension)
        } else {
            format!("{}-{}", file_stem, suffix)
        };
        Ok(self.as_ref().with_file_name(file_name))
    }

    fn read_dir<P>(&self, predicate: P) -> CIResult<Vec<PathBuf>>
    where
        P: FnMut(&PathBuf) -> bool,
    {
        let path = self.as_ref();
        Ok(path
            .read_dir()
            .with_context(|| format!("failed to read directory `{}`", path.display()))?
            .filter_map(|e| e.ok())
            .map(|e| e.path())
            .filter(predicate)
            .collect::<Vec<_>>())
    }
}
