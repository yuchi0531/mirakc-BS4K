use chrono::DateTime;
use chrono::Duration;
use chrono_jst::Jst;
use serde::Deserialize;
use serde::Serialize;

use crate::models::Eid;
use crate::models::Nid;
use crate::models::ServiceId;
use crate::models::Sid;
use crate::models::Tsid;

#[derive(Clone, Debug, Eq, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EitSection {
    pub original_network_id: Nid,
    pub transport_stream_id: Tsid,
    pub service_id: Sid,
    pub table_id: u16,
    pub section_number: u8,
    pub last_section_number: u8,
    pub segment_last_section_number: u8,
    pub version_number: u8,
    pub events: Vec<EitEvent>,
}

impl EitSection {
    pub fn is_valid(&self) -> bool {
        // EIT[schedule] covers 0x50-0x57 (basic) and 0x58-0x5F (extended).
        // BS4K MH-EIT sections are normalized by mirakc-arib into this same
        // range (MH 0x8B -> 0x50, basic 0x8C-0x93 -> 0x50-0x57,
        // extended 0x94-0x9B -> 0x58-0x5F), so the full range must be
        // accepted here.  Restricting to 0x50/0x51/0x58/0x59 drops most of
        // the schedule.
        matches!(self.table_id, 0x50..=0x5F)
    }

    pub fn is_basic(&self) -> bool {
        match self.table_id {
            0x50..=0x57 => true,
            0x58..=0x5F => false,
            _ => panic!("Invalid table_id"),
        }
    }

    pub fn table_index(&self) -> usize {
        self.table_id as usize - 0x50
    }

    pub fn segment_index(&self) -> usize {
        self.section_number as usize / 8
    }

    pub fn section_index(&self) -> usize {
        self.section_number as usize % 8
    }

    pub fn last_section_index(&self) -> usize {
        self.segment_last_section_number as usize % 8
    }

    pub fn service_id(&self) -> ServiceId {
        ServiceId::new(self.original_network_id, self.service_id)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn section_with_table_id(table_id: u16) -> EitSection {
        EitSection {
            original_network_id: 0.into(),
            transport_stream_id: 0.into(),
            service_id: 0.into(),
            table_id,
            section_number: 0,
            last_section_number: 0,
            segment_last_section_number: 0,
            version_number: 0,
            events: vec![],
        }
    }

    #[test]
    fn test_eit_section_is_valid_full_schedule_range() {
        // Previously only 0x50/0x51/0x58/0x59 were accepted, dropping most
        // of the schedule.  The full EIT[schedule] range must be valid,
        // including the MH-normalized BS4K tables.
        for table_id in 0x50..=0x5F {
            assert!(
                section_with_table_id(table_id).is_valid(),
                "table_id {table_id:#X} must be valid"
            );
        }
        assert!(!section_with_table_id(0x4E).is_valid());
        assert!(!section_with_table_id(0x4F).is_valid());
        assert!(!section_with_table_id(0x00).is_valid());
        assert!(!section_with_table_id(0x60).is_valid());
    }

    #[test]
    fn test_eit_section_basic_extended_split() {
        for table_id in 0x50..=0x57 {
            let section = section_with_table_id(table_id);
            assert!(section.is_basic(), "table_id {table_id:#X} must be basic");
        }
        for table_id in 0x58..=0x5F {
            let section = section_with_table_id(table_id);
            assert!(
                !section.is_basic(),
                "table_id {table_id:#X} must be extended"
            );
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EitEvent {
    pub event_id: Eid,
    pub start_time: Option<i64>, // UNIX time in milliseconds
    pub duration: Option<i64>,   // milliseconds
    pub scrambled: bool,
    pub descriptors: Vec<EitDescriptor>,
}

impl EitEvent {
    pub fn start_time(&self) -> Option<DateTime<Jst>> {
        self.start_time
            .and_then(DateTime::from_timestamp_millis)
            .map(|dt| dt.with_timezone(&Jst))
    }

    pub fn duration(&self) -> Option<Duration> {
        self.duration.and_then(Duration::try_milliseconds)
    }

    pub fn end_time(&self) -> Option<DateTime<Jst>> {
        match (self.start_time(), self.duration()) {
            (Some(start_time), Some(duration)) => Some(start_time + duration),
            _ => None,
        }
    }

    pub fn is_overnight_event(&self, midnight: DateTime<Jst>) -> bool {
        self.start_time().unwrap() < midnight && self.end_time().unwrap() > midnight
    }
}

#[derive(Clone, Debug, Eq, PartialEq, Deserialize, Serialize)]
#[serde(tag = "$type")]
pub enum EitDescriptor {
    #[serde(rename_all = "camelCase")]
    ShortEvent {
        #[serde(default, skip_serializing_if = "Option::is_none")]
        event_name: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        text: Option<String>,
    },
    Component(ComponentDescriptor),
    AudioComponent(AudioComponentDescriptor),
    #[serde(rename_all = "camelCase")]
    Content {
        nibbles: Vec<(u8, u8, u8, u8)>,
    },
    Series(SeriesDescriptor),
    EventGroup(EventGroupDescriptor),
    #[serde(rename_all = "camelCase")]
    ExtendedEvent {
        items: Vec<(String, String)>,
    },
}

#[derive(Clone, Debug, Eq, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ComponentDescriptor {
    pub stream_content: u8,
    pub component_type: u8,
    pub component_tag: u8,
    pub language_code: u32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub text: Option<String>,
}

#[derive(Clone, Debug, Eq, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AudioComponentDescriptor {
    pub stream_content: u8,
    pub component_type: u8,
    pub component_tag: u8,
    pub simulcast_group_tag: u8,
    pub es_multi_lingual_flag: bool,
    pub main_component_flag: bool,
    pub quality_indicator: u8,
    pub sampling_rate: u8,
    pub language_code: u32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub language_code2: Option<u32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub text: Option<String>,
}

#[derive(Clone, Debug, Eq, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SeriesDescriptor {
    pub series_id: u16,
    pub repeat_label: u8,
    pub program_pattern: u8,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub expire_date: Option<i64>,
    pub episode_number: u16,
    pub last_episode_number: u16,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub series_name: Option<String>,
}

#[derive(Clone, Debug, Eq, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EventGroupDescriptor {
    pub group_type: u8,
    pub events: Vec<EventGroupEvent>,
}

#[derive(Clone, Debug, Eq, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EventGroupEvent {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub original_network_id: Option<Nid>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub transport_stream_id: Option<Tsid>,
    pub service_id: Sid,
    pub event_id: Eid,
}
