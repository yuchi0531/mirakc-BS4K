use std::collections::HashMap;

use crate::config::FilterConfig;
use crate::config::PostFilterConfig;
use crate::config::PreFilterConfig;
use crate::error::Error;
use crate::models::ChannelType;

/// Returns true for channels whose tuner output must be passed through
/// without TS-oriented builtin filters.
///
/// BS4K tuners deliver decoded TLV (sync 0x7F, 1 TLV = 1 service, MMirakurun
/// compatible) straight from a remote server.  `mirakc-arib`
/// service/program/decode filters are TS-only and must not touch the stream.
/// Pre/post filters explicitly listed by the user are still applied.
pub fn is_tlv_passthrough(channel_type: ChannelType) -> bool {
    matches!(channel_type, ChannelType::BS4K)
}

/// Error message for BS4K program-level operations that require a PCR clock.
///
/// BS4K clock synchronization is disabled (see `epg::clock_synchronizer`):
/// TLV has no PCR clock concept.  Program-level streaming and recording need
/// `QueryClock`, which always fails with `ClockNotSynced` for BS4K.  Use
/// channel/service passthrough streams instead.
pub const BS4K_PROGRAM_LEVEL_UNSUPPORTED: &str = "BS4K program-level stream is unsupported (clock sync disabled for BS4K): \
     use channel/service passthrough streams only";

/// Returns an error for BS4K program-level operations, `Ok` otherwise.
///
/// Call this before `QueryClock` so that BS4K callers get a clear message
/// instead of a bare `ClockNotSynced` failure.
pub fn ensure_program_level_supported(channel_type: ChannelType) -> Result<(), Error> {
    if is_tlv_passthrough(channel_type) {
        return Err(Error::InvalidRequest(
            BS4K_PROGRAM_LEVEL_UNSUPPORTED.to_string(),
        ));
    }
    Ok(())
}

pub struct FilterPipelineBuilder {
    data: mustache::Data,
    filters: Vec<String>,
    content_type: String,
    seekable: bool,
}

impl FilterPipelineBuilder {
    pub fn new(data: mustache::Data, seekable: bool) -> Self {
        FilterPipelineBuilder {
            data,
            filters: Vec::new(),
            // BS4K TLV passthrough also keeps `video/MP2T` for now.  The
            // remote decoded-TLV server answers with `video/MP2T` and
            // Mirakurun/EPGStation clients expect it, so switching to a
            // TLV-specific content type would break compatibility.
            content_type: "video/MP2T".to_string(),
            seekable,
        }
    }

    pub fn build(self) -> (Vec<String>, String, bool) {
        (self.filters, self.content_type, self.seekable)
    }

    pub fn add_pre_filters(
        &mut self,
        pre_filters: &HashMap<String, PreFilterConfig>,
        names: &[String],
    ) -> Result<usize, Error> {
        for name in names.iter() {
            if pre_filters.contains_key(name) {
                self.add_pre_filter(&pre_filters[name], name)?;
            } else {
                tracing::warn!(pre_filter = name, "No such pre-filter");
            }
        }
        Ok(self.filters.len())
    }

    pub fn add_service_filter(&mut self, config: &FilterConfig) -> Result<usize, Error> {
        self.add_builtin_filter(config, "service-filter")
    }

    pub fn add_decode_filter(&mut self, config: &FilterConfig) -> Result<usize, Error> {
        self.add_builtin_filter(config, "decode-filter")
    }

    pub fn add_program_filter(&mut self, config: &FilterConfig) -> Result<usize, Error> {
        self.add_builtin_filter(config, "program-filter")
    }

    pub fn add_post_filters(
        &mut self,
        post_filters: &HashMap<String, PostFilterConfig>,
        names: &[String],
    ) -> Result<usize, Error> {
        for name in names.iter() {
            if post_filters.contains_key(name) {
                self.add_post_filter(&post_filters[name], name)?;
            } else {
                tracing::warn!(post_filter = name, "No such post-filter");
            }
        }
        Ok(self.filters.len())
    }

    fn add_pre_filter(&mut self, config: &PreFilterConfig, name: &str) -> Result<usize, Error> {
        if config.command.is_empty() {
            return Ok(self.filters.len());
        }
        let filter = match self.make_filter(&config.command) {
            Ok(filter) => filter,
            Err(err) => {
                tracing::error!(%err, pre_filter = name, "Failed to render pre-filter");
                return Err(err);
            }
        };
        if filter.is_empty() {
            tracing::warn!(pre_filter = name, "Empty pre-filter");
        } else {
            self.filters.push(filter);
            if !config.seekable {
                self.seekable = false;
            }
        }
        Ok(self.filters.len())
    }

    fn add_builtin_filter(&mut self, config: &FilterConfig, name: &str) -> Result<usize, Error> {
        if config.command.is_empty() {
            return Ok(self.filters.len());
        }
        let filter = match self.make_filter(&config.command) {
            Ok(filter) => filter,
            Err(err) => {
                tracing::error!(%err, filter = name, "Failed to render filter");
                return Err(err);
            }
        };
        if filter.is_empty() {
            tracing::warn!(filter = name, "Empty filter");
        } else {
            self.filters.push(filter);
            self.seekable = false;
        }
        Ok(self.filters.len())
    }

    fn add_post_filter(&mut self, config: &PostFilterConfig, name: &str) -> Result<usize, Error> {
        if config.command.is_empty() {
            return Ok(self.filters.len());
        }
        let filter = match self.make_filter(&config.command) {
            Ok(filter) => filter,
            Err(err) => {
                tracing::error!(%err, post_filter = name, "Failed to render post-filter");
                return Err(err);
            }
        };
        if filter.is_empty() {
            tracing::warn!(post_filter = name, "Empty post-filter");
        } else {
            self.filters.push(filter);
            if let Some(content_type) = config.content_type.as_ref() {
                self.content_type.clone_from(content_type);
            }
            if !config.seekable {
                self.seekable = false;
            }
        }
        Ok(self.filters.len())
    }

    fn make_filter(&self, command: &str) -> Result<String, Error> {
        let template = mustache::compile_str(command)?;
        Ok(template
            .render_data_to_string(&self.data)?
            .trim()
            .to_string())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use assert_matches::assert_matches;
    use test_log::test;

    #[test]
    fn test_is_tlv_passthrough() {
        assert!(is_tlv_passthrough(ChannelType::BS4K));
        assert!(!is_tlv_passthrough(ChannelType::GR));
        assert!(!is_tlv_passthrough(ChannelType::BS));
        assert!(!is_tlv_passthrough(ChannelType::CS));
        assert!(!is_tlv_passthrough(ChannelType::SKY));
    }

    #[test]
    fn test_ensure_program_level_supported() {
        assert_matches!(
            ensure_program_level_supported(ChannelType::BS4K),
            Err(Error::InvalidRequest(msg)) => {
                assert!(msg.contains("BS4K"));
            }
        );
        assert!(ensure_program_level_supported(ChannelType::GR).is_ok());
        assert!(ensure_program_level_supported(ChannelType::BS).is_ok());
        assert!(ensure_program_level_supported(ChannelType::CS).is_ok());
        assert!(ensure_program_level_supported(ChannelType::SKY).is_ok());
    }

    #[test]
    fn test_make_filter() {
        let channel = channel_gr!("test", "channel");
        let user = tuner_user!(0, web; "user-id");

        let data = mustache::MapBuilder::new()
            .insert_str("channel_name", &channel.name)
            .insert("channel_type", &channel.channel_type)
            .unwrap()
            .insert_str("channel", &channel.channel)
            .insert("user", &user)
            .unwrap()
            .build();

        let builder = FilterPipelineBuilder::new(data, false);

        assert_matches!(builder.make_filter("{{channel_name}}"), Ok(cmd) => {
            assert_eq!(cmd, "test");
        });
        assert_matches!(builder.make_filter("{{channel_type}}"), Ok(cmd) => {
            assert_eq!(cmd, "GR");
        });
        assert_matches!(builder.make_filter("{{channel}}"), Ok(cmd) => {
            assert_eq!(cmd, "channel");
        });
        assert_matches!(builder.make_filter("{{#user}}{{priority}}{{/user}}"), Ok(cmd) => {
            assert_eq!(cmd, "0");
        });
        // rust-mustache seems to support directly accessing properties.
        assert_matches!(builder.make_filter("{{user.priority}}"), Ok(cmd) => {
            assert_eq!(cmd, "0");
        });
        assert_matches!(builder.make_filter("{{user.info.Web.id}}"), Ok(cmd) => {
            assert_eq!(cmd, "user-id");
        });
        assert_matches!(builder.make_filter("{{^user.info.Job}}not job{{/user.info.Job}}"), Ok(cmd) => {
            assert_eq!(cmd, "not job");
        });
    }
}
